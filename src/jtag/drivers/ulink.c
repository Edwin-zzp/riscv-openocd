/***************************************************************************
 *   Copyright (C) 2011 by Martin Schmoelzer                               *
 *   <martin.schmoelzer@student.tuwien.ac.at>                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include <jtag/commands.h>
#include <target/image.h>
#include <helper/types.h>
#include "usb_common.h"
#include "OpenULINK/include/msgtypes.h"

/** USB Vendor ID of ULINK device in unconfigured state (no firmware loaded
 *  yet) or with OpenULINK firmware. */
#define ULINK_VID                0xC251

/** USB Product ID of ULINK device in unconfigured state (no firmware loaded
 *  yet) or with OpenULINK firmware. */
#define ULINK_PID                0x2710

/** Address of EZ-USB CPU Control & Status register. This register can be
 *  written by issuing a Control EP0 vendor request. */
#define CPUCS_REG                0x7F92

/** USB Control EP0 bRequest: "Firmware Load". */
#define REQUEST_FIRMWARE_LOAD    0xA0

/** Value to write into CPUCS to put EZ-USB into reset. */
#define CPU_RESET                0x01

/** Value to write into CPUCS to put EZ-USB out of reset. */
#define CPU_START                0x00

/** Base address of firmware in EZ-USB code space. */
#define FIRMWARE_ADDR            0x0000

/** USB interface number */
#define USB_INTERFACE            0

/** libusb timeout in ms */
#define USB_TIMEOUT              5000

/** Delay (in microseconds) to wait while EZ-USB performs ReNumeration. */
#define ULINK_RENUMERATION_DELAY 1500000

/** Location of OpenULINK firmware image. TODO: Provide some way of modifying
 *  this path, maybe in a separate OpenOCD command? */
#define ULINK_FIRMWARE_FILE      PKGLIBDIR "/OpenULINK/ulink_firmware.hex"

/** Maximum size of a single firmware section. Entire EZ-USB code space = 8kB */
#define SECTION_BUFFERSIZE       8192

/** Tuning of OpenOCD SCAN commands split into multiple OpenULINK commands. */
#define SPLIT_SCAN_THRESHOLD     10

/** ULINK hardware type */
enum ulink_type
{
  /** Original ULINK adapter, based on Cypress EZ-USB (AN2131):
   *  Full JTAG support, no SWD support. */
  ULINK_1,

  /** Newer ULINK adapter, based on NXP LPC2148. Currently unsupported. */
  ULINK_2,

  /** Newer ULINK adapter, based on EZ-USB FX2 + FPGA. Currently unsupported. */
  ULINK_PRO,

  /** Newer ULINK adapter, possibly based on ULINK 2. Currently unsupported. */
  ULINK_ME
};

enum ulink_payload_direction
{
  PAYLOAD_DIRECTION_OUT,
  PAYLOAD_DIRECTION_IN
};

/**
 * OpenULINK command (OpenULINK command queue element).
 *
 * For the OUT direction payload, things are quite easy: Payload is stored
 * in a rather small array (up to 63 bytes), the payload is always allocated
 * by the function generating the command and freed by ulink_clear_queue().
 *
 * For the IN direction payload, things get a little bit more complicated:
 * The maximum IN payload size for a single command is 64 bytes. Assume that
 * a single OpenOCD command needs to scan 256 bytes. This results in the
 * generation of four OpenULINK commands. The function generating these
 * commands shall allocate an uint8_t[256] array. Each command's #payload_in
 * pointer shall point to the corresponding offset where IN data shall be
 * placed, while #payload_in_start shall point to the first element of the 256
 * byte array.
 * - first command:  #payload_in_start + 0
 * - second command: #payload_in_start + 64
 * - third command:  #payload_in_start + 128
 * - fourth command: #payload_in_start + 192
 *
 * The last command sets #needs_postprocessing to true.
 */
struct ulink_cmd {
  uint8_t id;                 ///< ULINK command ID

  uint8_t *payload_out;       ///< OUT direction payload data
  uint8_t payload_out_size;   ///< OUT direction payload size for this command

  uint8_t *payload_in_start;  ///< Pointer to first element of IN payload array
  uint8_t *payload_in;        ///< Pointer where IN payload shall be stored
  uint8_t payload_in_size;    ///< IN direction payload size for this command

  /** Indicates if this command needs post-processing */
  bool needs_postprocessing;

  /** Indicates if ulink_clear_queue() should free payload_in_start  */
  bool free_payload_in_start;

  /** Pointer to corresponding OpenOCD command for post-processing */
  struct jtag_command *cmd_origin;

  struct ulink_cmd *next;     ///< Pointer to next command (linked list)
};

typedef struct ulink_cmd ulink_cmd_t;

/** Describes one driver instance */
struct ulink
{
  struct usb_dev_handle *usb_handle;
  enum ulink_type type;

  int commands_in_queue;     ///< Number of commands in queue
  ulink_cmd_t *queue_start;  ///< Pointer to first command in queue
  ulink_cmd_t *queue_end;    ///< Pointer to last command in queue
};

/**************************** Function Prototypes *****************************/

/* USB helper functions */
int ulink_usb_open(struct ulink **device);
int ulink_usb_close(struct ulink **device);

/* ULINK MCU (Cypress EZ-USB) specific functions */
int ulink_cpu_reset(struct ulink *device, char reset_bit);
int ulink_load_firmware_and_renumerate(struct ulink **device, char *filename,
    uint32_t delay);
int ulink_load_firmware(struct ulink *device, char *filename);
int ulink_write_firmware_section(struct ulink *device,
    struct image *firmware_image, int section_index);

/* Generic helper functions */
void ulink_print_signal_states(uint8_t input_signals, uint8_t output_signals);

/* OpenULINK command generation helper functions */
int ulink_allocate_payload(ulink_cmd_t *ulink_cmd, int size,
    enum ulink_payload_direction direction);

/* OpenULINK command queue helper functions */
int ulink_get_queue_size(struct ulink *device,
    enum ulink_payload_direction direction);
void ulink_clear_queue(struct ulink *device);
int ulink_append_queue(struct ulink *device, ulink_cmd_t *ulink_cmd);
int ulink_execute_queued_commands(struct ulink *device, int timeout);

#ifdef _DEBUG_JTAG_IO_
const char * ulink_cmd_id_string(uint8_t id);
void ulink_print_command(ulink_cmd_t *ulink_cmd);
void ulink_print_queue(struct ulink *device);
#endif

int ulink_append_scan_cmd(struct ulink *device, enum scan_type scan_type,
    int scan_size_bits, uint8_t *tdi, uint8_t *tdo_start, uint8_t *tdo,
    uint8_t tms_count_start, uint8_t tms_sequence_start, uint8_t tms_count_end,
    uint8_t tms_sequence_end, struct jtag_command *origin, bool postprocess);
int ulink_append_clock_tms_cmd(struct ulink *device, uint8_t count,
    uint8_t sequence);
int ulink_append_clock_tck_cmd(struct ulink *device, uint16_t count);
int ulink_append_get_signals_cmd(struct ulink *device);
int ulink_append_set_signals_cmd(struct ulink *device, uint8_t low,
    uint8_t high);
int ulink_append_sleep_cmd(struct ulink *device, uint32_t us);
int ulink_append_configure_tck_cmd(struct ulink *device, uint8_t delay_scan,
    uint8_t delay_tck, uint8_t delay_tms);
int ulink_append_led_cmd(struct ulink *device, uint8_t led_state);
int ulink_append_test_cmd(struct ulink *device);

/* Interface between OpenULINK and OpenOCD */
static void ulink_set_end_state(tap_state_t endstate);
int ulink_queue_statemove(struct ulink *device);

int ulink_queue_scan(struct ulink *device, struct jtag_command *cmd);
int ulink_queue_tlr_reset(struct ulink *device, struct jtag_command *cmd);
int ulink_queue_runtest(struct ulink *device, struct jtag_command *cmd);
int ulink_queue_reset(struct ulink *device, struct jtag_command *cmd);
int ulink_queue_pathmove(struct ulink *device, struct jtag_command *cmd);
int ulink_queue_sleep(struct ulink *device, struct jtag_command *cmd);

int ulink_post_process_scan(ulink_cmd_t *ulink_cmd);
int ulink_post_process_queue(struct ulink *device);

/* JTAG driver functions (registered in struct jtag_interface) */
static int ulink_execute_queue(void);
static int ulink_khz(int khz, int *jtag_speed);
static int ulink_speed(int speed);
static int ulink_speed_div(int speed, int *khz);
static int ulink_init(void);
static int ulink_quit(void);

/****************************** Global Variables ******************************/

struct ulink *ulink_handle;

/**************************** USB helper functions ****************************/

/**
 * Opens the ULINK device and claims its USB interface.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_usb_open(struct ulink **device)
{
  int ret;
  struct usb_dev_handle *usb_handle;

  /* Currently, only original ULINK is supported */
  uint16_t vids[] = { ULINK_VID, 0 };
  uint16_t pids[] = { ULINK_PID, 0 };

  ret = jtag_usb_open(vids, pids, &usb_handle);

  if (ret != ERROR_OK) {
    return ret;
  }

  ret = usb_claim_interface(usb_handle, 0);

  if (ret != 0) {
    return ret;
  }

  (*device)->usb_handle = usb_handle;
  (*device)->type = ULINK_1;

  return ERROR_OK;
}

/**
 * Releases the ULINK interface and closes the USB device handle.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_usb_close(struct ulink **device)
{
  if (usb_release_interface((*device)->usb_handle, 0) != 0) {
    return ERROR_FAIL;
  }

  if (usb_close((*device)->usb_handle) != 0) {
    return ERROR_FAIL;
  }

  (*device)->usb_handle = NULL;

  return ERROR_OK;
}

/******************* ULINK CPU (EZ-USB) specific functions ********************/

/**
 * Writes '0' or '1' to the CPUCS register, putting the EZ-USB CPU into reset
 * or out of reset.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param reset_bit 0 to put CPU into reset, 1 to put CPU out of reset.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_cpu_reset(struct ulink *device, char reset_bit)
{
  int ret;

  ret = usb_control_msg(device->usb_handle,
      (USB_ENDPOINT_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE),
      REQUEST_FIRMWARE_LOAD, CPUCS_REG, 0, &reset_bit, 1, USB_TIMEOUT);

  /* usb_control_msg() returns the number of bytes transferred during the
   * DATA stage of the control transfer - must be exactly 1 in this case! */
  if (ret != 1) {
    return ERROR_FAIL;
  }
  return ERROR_OK;
}

/**
 * Puts the ULINK's EZ-USB microcontroller into reset state, downloads
 * the firmware image, resumes the microcontroller and re-enumerates
 * USB devices.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 *  The usb_handle member will be modified during re-enumeration.
 * @param filename path to the Intel HEX file containing the firmware image.
 * @param delay the delay to wait for the device to re-enumerate.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_load_firmware_and_renumerate(struct ulink **device,
    char *filename, uint32_t delay)
{
  int ret;

  /* Basic process: After downloading the firmware, the ULINK will disconnect
   * itself and re-connect after a short amount of time so we have to close
   * the handle and re-enumerate USB devices */

  ret = ulink_load_firmware(*device, filename);
  if (ret != ERROR_OK) {
    return ret;
  }

  ret = ulink_usb_close(device);
  if (ret != ERROR_OK) {
    return ret;
  }

  usleep(delay);

  ret = ulink_usb_open(device);
  if (ret != ERROR_OK) {
    return ret;
  }

  return ERROR_OK;
}

/**
 * Downloads a firmware image to the ULINK's EZ-USB microcontroller
 * over the USB bus.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param filename an absolute or relative path to the Intel HEX file
 *  containing the firmware image.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_load_firmware(struct ulink *device, char *filename)
{
  struct image ulink_firmware_image;
  int ret, i;

  ret = ulink_cpu_reset(device, CPU_RESET);
  if (ret != ERROR_OK) {
    LOG_ERROR("Could not halt ULINK CPU");
    return ret;
  }

  ulink_firmware_image.base_address = 0;
  ulink_firmware_image.base_address_set = 0;

  ret = image_open(&ulink_firmware_image, ULINK_FIRMWARE_FILE, "ihex");
  if (ret != ERROR_OK) {
    return ret;
  }

  /* Download all sections in the image to ULINK */
  for (i = 0; i < ulink_firmware_image.num_sections; i++) {
    ret = ulink_write_firmware_section(device, &ulink_firmware_image, i);
    if (ret != ERROR_OK) {
      return ret;
    }
  }

  image_close(&ulink_firmware_image);

  ret = ulink_cpu_reset(device, CPU_START);
  if (ret != ERROR_OK) {
    LOG_ERROR("Could not restart ULINK CPU");
    return ret;
  }

  return ERROR_OK;
}

/**
 * Send one contiguous firmware section to the ULINK's EZ-USB microcontroller
 * over the USB bus.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param firmware_image pointer to the firmware image that contains the section
 *  which should be sent to the ULINK's EZ-USB microcontroller.
 * @param section_index index of the section within the firmware image.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_write_firmware_section(struct ulink *device,
    struct image *firmware_image, int section_index)
{
  uint16_t addr, size, bytes_remaining, chunk_size;
  uint8_t data[SECTION_BUFFERSIZE];
  uint8_t *data_ptr = data;
  size_t size_read;
  int ret;

  size = (uint16_t)firmware_image->sections[section_index].size;
  addr = (uint16_t)firmware_image->sections[section_index].base_address;

  LOG_DEBUG("section %02i at addr 0x%04x (size 0x%04x)", section_index, addr,
      size);

  if (data == NULL) {
    return ERROR_FAIL;
  }

  /* Copy section contents to local buffer */
  ret = image_read_section(firmware_image, section_index, 0, size, data,
      &size_read);

  if ((ret != ERROR_OK) || (size_read != size)) {
    /* Propagating the return code would return '0' (misleadingly indicating
     * successful execution of the function) if only the size check fails. */
    return ERROR_FAIL;
  }

  bytes_remaining = size;

  /* Send section data in chunks of up to 64 bytes to ULINK */
  while (bytes_remaining > 0) {
    if (bytes_remaining > 64) {
      chunk_size = 64;
    }
    else {
      chunk_size = bytes_remaining;
    }

    ret = usb_control_msg(device->usb_handle,
        (USB_ENDPOINT_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE),
        REQUEST_FIRMWARE_LOAD, addr, FIRMWARE_ADDR, (char *)data_ptr,
        chunk_size, USB_TIMEOUT);

    if (ret != (int)chunk_size) {
      /* Abort if libusb sent less data than requested */
      return ERROR_FAIL;
    }

    bytes_remaining -= chunk_size;
    addr += chunk_size;
    data_ptr += chunk_size;
  }

  return ERROR_OK;
}

/************************** Generic helper functions **************************/

/**
 * Print state of interesting signals via LOG_INFO().
 *
 * @param input_signals input signal states as returned by CMD_GET_SIGNALS
 * @param output_signals output signal states as returned by CMD_GET_SIGNALS
 */
void ulink_print_signal_states(uint8_t input_signals, uint8_t output_signals)
{
  LOG_INFO("ULINK signal states: TDI: %i, TDO: %i, TMS: %i, TCK: %i, TRST: %i,"
      " SRST: %i",
      (output_signals & SIGNAL_TDI   ? 1 : 0),
      (input_signals  & SIGNAL_TDO   ? 1 : 0),
      (output_signals & SIGNAL_TMS   ? 1 : 0),
      (output_signals & SIGNAL_TCK   ? 1 : 0),
      (output_signals & SIGNAL_TRST  ? 0 : 1),  // TRST and RESET are inverted
      (output_signals & SIGNAL_RESET ? 0 : 1)); // by hardware
}

/**************** OpenULINK command generation helper functions ***************/

/**
 * Allocate and initialize space in memory for OpenULINK command payload.
 *
 * @param ulink_cmd pointer to command whose payload should be allocated.
 * @param size the amount of memory to allocate (bytes).
 * @param direction which payload to allocate.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_allocate_payload(ulink_cmd_t *ulink_cmd, int size,
    enum ulink_payload_direction direction)
{
  uint8_t *payload;

  payload = calloc(size, sizeof(uint8_t));

  if (payload == NULL) {
    LOG_ERROR("Could not allocate OpenULINK command payload: out of memory");
    return ERROR_FAIL;
  }

  switch (direction) {
  case PAYLOAD_DIRECTION_OUT:
    if (ulink_cmd->payload_out != NULL) {
      LOG_ERROR("BUG: Duplicate payload allocation for OpenULINK command");
      return ERROR_FAIL;
    }
    else {
      ulink_cmd->payload_out = payload;
      ulink_cmd->payload_out_size = size;
    }
    break;
  case PAYLOAD_DIRECTION_IN:
    if (ulink_cmd->payload_in_start != NULL) {
      LOG_ERROR("BUG: Duplicate payload allocation for OpenULINK command");
      return ERROR_FAIL;
    }
    else {
      ulink_cmd->payload_in_start = payload;
      ulink_cmd->payload_in = payload;
      ulink_cmd->payload_in_size = size;

      /* By default, free payload_in_start in ulink_clear_queue(). Commands
       * that do not want this behavior (e. g. split scans) must turn it off
       * separately! */
      ulink_cmd->free_payload_in_start = true;
    }
    break;
  }

  return ERROR_OK;
}

/****************** OpenULINK command queue helper functions ******************/

/**
 * Get the current number of bytes in the queue, including command IDs.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param direction the transfer direction for which to get byte count.
 * @return the number of bytes currently stored in the queue for the specified
 *  direction.
 */
int ulink_get_queue_size(struct ulink *device,
    enum ulink_payload_direction direction)
{
  ulink_cmd_t *current = device->queue_start;
  int sum = 0;

  while (current != NULL) {
    switch (direction) {
    case PAYLOAD_DIRECTION_OUT:
      sum += current->payload_out_size + 1; // + 1 byte for Command ID
      break;
    case PAYLOAD_DIRECTION_IN:
      sum += current->payload_in_size;
      break;
    }

    current = current->next;
  }

  return sum;
}

/**
 * Clear the OpenULINK command queue.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
void ulink_clear_queue(struct ulink *device)
{
  ulink_cmd_t *current = device->queue_start;
  ulink_cmd_t *next = NULL;

  while (current != NULL) {
    /* Save pointer to next element */
    next = current->next;

    /* Free payloads: OUT payload can be freed immediately */
    free(current->payload_out);
    current->payload_out = NULL;

    /* IN payload MUST be freed ONLY if no other commands use the
     * payload_in_start buffer */
    if (current->free_payload_in_start == true) {
      free(current->payload_in_start);
      current->payload_in_start = NULL;
      current->payload_in = NULL;
    }

    /* Free queue element */
    free(current);

    /* Proceed with next element */
    current = next;
  }

  device->commands_in_queue = 0;
  device->queue_start = NULL;
  device->queue_end = NULL;
}

/**
 * Add a command to the OpenULINK command queue.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param ulink_cmd pointer to command that shall be appended to the OpenULINK
 *  command queue.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_queue(struct ulink *device, ulink_cmd_t *ulink_cmd)
{
  int newsize_out, newsize_in;
  int ret;

  newsize_out = ulink_get_queue_size(device, PAYLOAD_DIRECTION_OUT) + 1
      + ulink_cmd->payload_out_size;

  newsize_in = ulink_get_queue_size(device, PAYLOAD_DIRECTION_IN)
      + ulink_cmd->payload_in_size;

  /* Check if the current command can be appended to the queue */
  if ((newsize_out > 64) || (newsize_in > 64)) {
    /* New command does not fit. Execute all commands in queue before starting
     * new queue with the current command as first entry. */
    ret = ulink_execute_queued_commands(device, USB_TIMEOUT);
    if (ret != ERROR_OK) {
      return ret;
    }

    ret = ulink_post_process_queue(device);
    if (ret != ERROR_OK) {
      return ret;
    }

    ulink_clear_queue(device);
  }

  if (device->queue_start == NULL) {
    /* Queue was empty */
    device->commands_in_queue = 1;

    device->queue_start = ulink_cmd;
    device->queue_end = ulink_cmd;
  }
  else {
    /* There are already commands in the queue */
    device->commands_in_queue++;

    device->queue_end->next = ulink_cmd;
    device->queue_end = ulink_cmd;
  }

  return ERROR_OK;
}

/**
 * Sends all queued OpenULINK commands to the ULINK for execution.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_execute_queued_commands(struct ulink *device, int timeout)
{
  ulink_cmd_t *current;
  int ret, i, index_out, index_in, count_out, count_in;
  uint8_t buffer[64];

#ifdef _DEBUG_JTAG_IO_
  ulink_print_queue(device);
#endif

  index_out = 0;
  count_out = 0;
  count_in = 0;

  for (current = device->queue_start; current; current = current->next) {
    /* Add command to packet */
    buffer[index_out] = current->id;
    index_out++;
    count_out++;

    for (i = 0; i < current->payload_out_size; i++) {
      buffer[index_out + i] = current->payload_out[i];
    }
    index_out += current->payload_out_size;
    count_in += current->payload_in_size;
    count_out += current->payload_out_size;
  }

  /* Send packet to ULINK */
  ret = usb_bulk_write(device->usb_handle, (2 | USB_ENDPOINT_OUT),
      (char *)buffer, count_out, timeout);
  if (ret < 0) {
    return ERROR_FAIL;
  }
  if (ret != count_out) {
    return ERROR_FAIL;
  }

  /* Wait for response if commands contain IN payload data */
  if (count_in > 0) {
    ret = usb_bulk_read(device->usb_handle, (2 | USB_ENDPOINT_IN),
        (char *)buffer, 64, timeout);
    if (ret < 0) {
      return ERROR_FAIL;
    }
    if (ret != count_in) {
      return ERROR_FAIL;
    }

    /* Write back IN payload data */
    index_in = 0;
    for (current = device->queue_start; current; current = current->next) {
      for (i = 0; i < current->payload_in_size; i++) {
        current->payload_in[i] = buffer[index_in];
        index_in++;
      }
    }
  }

  return ERROR_OK;
}

#ifdef _DEBUG_JTAG_IO_

/**
 * Convert an OpenULINK command ID (\a id) to a human-readable string.
 *
 * @param id the OpenULINK command ID.
 * @return the corresponding human-readable string.
 */
const char * ulink_cmd_id_string(uint8_t id)
{
  switch (id) {
  case CMD_SCAN_IN:
    return "CMD_SCAN_IN";
    break;
  case CMD_SLOW_SCAN_IN:
    return "CMD_SLOW_SCAN_IN";
    break;
  case CMD_SCAN_OUT:
    return "CMD_SCAN_OUT";
    break;
  case CMD_SLOW_SCAN_OUT:
    return "CMD_SLOW_SCAN_OUT";
    break;
  case CMD_SCAN_IO:
    return "CMD_SCAN_IO";
    break;
  case CMD_SLOW_SCAN_IO:
    return "CMD_SLOW_SCAN_IO";
    break;
  case CMD_CLOCK_TMS:
    return "CMD_CLOCK_TMS";
    break;
  case CMD_SLOW_CLOCK_TMS:
    return "CMD_SLOW_CLOCK_TMS";
    break;
  case CMD_CLOCK_TCK:
    return "CMD_CLOCK_TCK";
    break;
  case CMD_SLEEP_US:
    return "CMD_SLEEP_US";
    break;
  case CMD_SLEEP_MS:
    return "CMD_SLEEP_MS";
    break;
  case CMD_GET_SIGNALS:
    return "CMD_GET_SIGNALS";
    break;
  case CMD_SET_SIGNALS:
    return "CMD_SET_SIGNALS";
    break;
  case CMD_CONFIGURE_TCK_FREQ:
    return "CMD_CONFIGURE_TCK_FREQ";
    break;
  case CMD_SET_LEDS:
    return "CMD_SET_LEDS";
    break;
  case CMD_TEST:
    return "CMD_TEST";
    break;
  default:
    return "CMD_UNKNOWN";
    break;
  }
}

/**
 * Print one OpenULINK command to stdout.
 *
 * @param ulink_cmd pointer to OpenULINK command.
 */
void ulink_print_command(ulink_cmd_t *ulink_cmd)
{
  int i;

  printf("  %-22s | OUT size = %i, bytes = 0x", ulink_cmd_id_string(ulink_cmd->id),
      ulink_cmd->payload_out_size);

  for (i = 0; i < ulink_cmd->payload_out_size; i++) {
    printf("%02X ", ulink_cmd->payload_out[i]);
  }
  printf("\n                         | IN size  = %i\n", ulink_cmd->payload_in_size);
}

/**
 * Print the OpenULINK command queue to stdout.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 */
void ulink_print_queue(struct ulink *device)
{
  ulink_cmd_t *current;

  printf("OpenULINK command queue:\n");

  for (current = device->queue_start; current; current = current->next) {
    ulink_print_command(current);
  }
}

#endif /* _DEBUG_JTAG_IO_ */

/**
 * Perform JTAG scan
 *
 * Creates and appends a JTAG scan command to the OpenULINK command queue.
 * A JTAG scan consists of three steps:
 * - Move to the desired SHIFT state, depending on scan type (IR/DR scan).
 * - Shift TDI data into the JTAG chain, optionally reading the TDO pin.
 * - Move to the desired end state.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param scan_type the type of the scan (IN, OUT, IO (bidirectional)).
 * @param scan_size_bits number of bits to shift into the JTAG chain.
 * @param tdi pointer to array containing TDI data.
 * @param tdo_start pointer to first element of array where TDO data shall be
 *  stored. See #ulink_cmd for details.
 * @param tdo pointer to array where TDO data shall be stored
 * @param tms_count_start number of TMS state transitions to perform BEFORE
 *  shifting data into the JTAG chain.
 * @param tms_sequence_start sequence of TMS state transitions that will be
 *  performed BEFORE shifting data into the JTAG chain.
 * @param tms_count_end number of TMS state transitions to perform AFTER
 *  shifting data into the JTAG chain.
 * @param tms_sequence_end sequence of TMS state transitions that will be
 *  performed AFTER shifting data into the JTAG chain.
 * @param origin pointer to OpenOCD command that generated this scan command.
 * @param postprocess whether this command needs to be post-processed after
 *  execution.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_scan_cmd(struct ulink *device, enum scan_type scan_type,
    int scan_size_bits, uint8_t *tdi, uint8_t *tdo_start, uint8_t *tdo,
    uint8_t tms_count_start, uint8_t tms_sequence_start, uint8_t tms_count_end,
    uint8_t tms_sequence_end, struct jtag_command *origin, bool postprocess)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret, i, scan_size_bytes;
  uint8_t bits_last_byte;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  /* Check size of command. USB buffer can hold 64 bytes, 1 byte is command ID,
   * 5 bytes are setup data -> 58 remaining payload bytes for TDI data */
  if (scan_size_bits > (58 * 8)) {
    LOG_ERROR("BUG: Tried to create CMD_SCAN_IO OpenULINK command with too"
        " large payload");
    return ERROR_FAIL;
  }

  scan_size_bytes = DIV_ROUND_UP(scan_size_bits, 8);

  bits_last_byte = scan_size_bits % 8;
  if (bits_last_byte == 0) {
    bits_last_byte = 8;
  }

  /* Allocate out_payload depending on scan type */
  // TODO: set command ID depending on interface speed settings (slow scan)
  switch (scan_type) {
  case SCAN_IN:
    cmd->id = CMD_SCAN_IN;
    ret = ulink_allocate_payload(cmd, 5, PAYLOAD_DIRECTION_OUT);
    break;
  case SCAN_OUT:
    cmd->id = CMD_SCAN_OUT;
    ret = ulink_allocate_payload(cmd, scan_size_bytes + 5, PAYLOAD_DIRECTION_OUT);
    break;
  case SCAN_IO:
    cmd->id = CMD_SCAN_IO;
    ret = ulink_allocate_payload(cmd, scan_size_bytes + 5, PAYLOAD_DIRECTION_OUT);
    break;
  default:
    LOG_ERROR("BUG: ulink_append_scan_cmd() encountered an unknown scan type");
    ret = ERROR_FAIL;
    break;
  }

  if (ret != ERROR_OK) {
    return ret;
  }

  /* Build payload_out that is common to all scan types */
  cmd->payload_out[0] = scan_size_bytes & 0xFF;
  cmd->payload_out[1] = bits_last_byte & 0xFF;
  cmd->payload_out[2] = ((tms_count_start & 0x0F) << 4) | (tms_count_end & 0x0F);
  cmd->payload_out[3] = tms_sequence_start;
  cmd->payload_out[4] = tms_sequence_end;

  /* Setup payload_out for types with OUT transfer */
  if ((scan_type == SCAN_OUT) || (scan_type == SCAN_IO)) {
    for (i = 0; i < scan_size_bytes; i++) {
      cmd->payload_out[i + 5] = tdi[i];
    }
  }

  /* Setup payload_in pointers for types with IN transfer */
  if ((scan_type == SCAN_IN) || (scan_type == SCAN_IO)) {
    cmd->payload_in_start = tdo_start;
    cmd->payload_in = tdo;
    cmd->payload_in_size = scan_size_bytes;
  }

  cmd->needs_postprocessing = postprocess;
  cmd->cmd_origin = origin;

  /* For scan commands, we free payload_in_start only when the command is
   * the last in a series of split commands or a stand-alone command */
  cmd->free_payload_in_start = postprocess;

  return ulink_append_queue(device, cmd);
}

/**
 * Perform TAP state transitions
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param count defines the number of TCK clock cycles generated (up to 8).
 * @param sequence defines the TMS pin levels for each state transition. The
 *  Least-Significant Bit is read first.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_clock_tms_cmd(struct ulink *device, uint8_t count,
    uint8_t sequence)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_CLOCK_TMS;

  /* CMD_CLOCK_TMS has two OUT payload bytes and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 2, PAYLOAD_DIRECTION_OUT);
  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = count;
  cmd->payload_out[1] = sequence;

  return ulink_append_queue(device, cmd);
}

/**
 * Generate a defined amount of TCK clock cycles
 *
 * All other JTAG signals are left unchanged.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param count the number of TCK clock cycles to generate.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_clock_tck_cmd(struct ulink *device, uint16_t count)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_CLOCK_TCK;

  /* CMD_CLOCK_TCK has two OUT payload bytes and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 2, PAYLOAD_DIRECTION_OUT);
  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = count & 0xff;
  cmd->payload_out[1] = (count >> 8) & 0xff;

  return ulink_append_queue(device, cmd);
}

/**
 * Read JTAG signals.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_get_signals_cmd(struct ulink *device)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_GET_SIGNALS;
  cmd->needs_postprocessing = true;

  /* CMD_GET_SIGNALS has two IN payload bytes */
  ret = ulink_allocate_payload(cmd, 2, PAYLOAD_DIRECTION_IN);

  if (ret != ERROR_OK) {
    return ret;
  }

  return ulink_append_queue(device, cmd);
}

/**
 * Arbitrarily set JTAG output signals.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param low defines which signals will be de-asserted. Each bit corresponds
 *  to a JTAG signal:
 *  - SIGNAL_TDI
 *  - SIGNAL_TMS
 *  - SIGNAL_TCK
 *  - SIGNAL_TRST
 *  - SIGNAL_BRKIN
 *  - SIGNAL_RESET
 *  - SIGNAL_OCDSE
 * @param high defines which signals will be asserted.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_set_signals_cmd(struct ulink *device, uint8_t low,
    uint8_t high)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_SET_SIGNALS;

  /* CMD_SET_SIGNALS has two OUT payload bytes and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 2, PAYLOAD_DIRECTION_OUT);

  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = low;
  cmd->payload_out[1] = high;

  return ulink_append_queue(device, cmd);
}

/**
 * Sleep for a pre-defined number of microseconds
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param us the number microseconds to sleep.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_sleep_cmd(struct ulink *device, uint32_t us)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_SLEEP_US;

  /* CMD_SLEEP_US has two OUT payload bytes and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 2, PAYLOAD_DIRECTION_OUT);

  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = us & 0x00ff;
  cmd->payload_out[1] = (us >> 8) & 0x00ff;

  return ulink_append_queue(device, cmd);
}

/**
 * Set TCK delay counters
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param delay_scan delay count top value in jtag_slow_scan() functions
 * @param delay_tck delay count top value in jtag_clock_tck() function
 * @param delay_tms delay count top value in jtag_slow_clock_tms() function
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_configure_tck_cmd(struct ulink *device, uint8_t delay_scan,
    uint8_t delay_tck, uint8_t delay_tms)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_CONFIGURE_TCK_FREQ;

  /* CMD_CONFIGURE_TCK_FREQ has three OUT payload bytes and zero
   * IN payload bytes */
  ret = ulink_allocate_payload(cmd, 3, PAYLOAD_DIRECTION_OUT);
  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = delay_scan;
  cmd->payload_out[1] = delay_tck;
  cmd->payload_out[2] = delay_tms;

  return ulink_append_queue(device, cmd);
}

/**
 * Turn on/off ULINK LEDs.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param led_state which LED(s) to turn on or off. The following bits
 *  influence the LEDS:
 *  - Bit 0: Turn COM LED on
 *  - Bit 1: Turn RUN LED on
 *  - Bit 2: Turn COM LED off
 *  - Bit 3: Turn RUN LED off
 *  If both the on-bit and the off-bit for the same LED is set, the LED is
 *  turned off.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_led_cmd(struct ulink *device, uint8_t led_state)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_SET_LEDS;

  /* CMD_SET_LEDS has one OUT payload byte and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 1, PAYLOAD_DIRECTION_OUT);
  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = led_state;

  return ulink_append_queue(device, cmd);
}

/**
 * Test command. Used to check if the ULINK device is ready to accept new
 * commands.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_append_test_cmd(struct ulink *device)
{
  ulink_cmd_t *cmd = calloc(1, sizeof(ulink_cmd_t));
  int ret;

  if (cmd == NULL) {
    return ERROR_FAIL;
  }

  cmd->id = CMD_TEST;

  /* CMD_TEST has one OUT payload byte and zero IN payload bytes */
  ret = ulink_allocate_payload(cmd, 1, PAYLOAD_DIRECTION_OUT);
  if (ret != ERROR_OK) {
    return ret;
  }

  cmd->payload_out[0] = 0xAA;

  return ulink_append_queue(device, cmd);
}

/******************* Interface between OpenULINK and OpenOCD ******************/

/**
 * Sets the end state follower (see interface.h) if \a endstate is a stable
 * state.
 *
 * @param endstate the state the end state follower should be set to.
 */
static void ulink_set_end_state(tap_state_t endstate)
{
  if (tap_is_state_stable(endstate)) {
    tap_set_end_state(endstate);
  }
  else {
    LOG_ERROR("BUG: %s is not a valid end state", tap_state_name(endstate));
    exit( EXIT_FAILURE);
  }
}

/**
 * Move from the current TAP state to the current TAP end state.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_statemove(struct ulink *device)
{
  uint8_t tms_sequence, tms_count;
  int ret;

  if (tap_get_state() == tap_get_end_state()) {
    /* Do nothing if we are already there */
    return ERROR_OK;
  }

  tms_sequence = tap_get_tms_path(tap_get_state(), tap_get_end_state());
  tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());

  ret = ulink_append_clock_tms_cmd(device, tms_count, tms_sequence);

  if (ret == ERROR_OK) {
    tap_set_state(tap_get_end_state());
  }

  return ret;
}

/**
 * Perform a scan operation on a JTAG register.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_scan(struct ulink *device, struct jtag_command *cmd)
{
  uint32_t scan_size_bits, scan_size_bytes, bits_last_scan;
  uint32_t scans_max_payload, bytecount;
  uint8_t *tdi_buffer_start = NULL, *tdi_buffer = NULL;
  uint8_t *tdo_buffer_start = NULL, *tdo_buffer = NULL;

  uint8_t first_tms_count, first_tms_sequence;
  uint8_t last_tms_count, last_tms_sequence;

  uint8_t tms_count_pause, tms_sequence_pause;
  uint8_t tms_count_resume, tms_sequence_resume;

  uint8_t tms_count_start, tms_sequence_start;
  uint8_t tms_count_end, tms_sequence_end;

  enum scan_type type;
  int ret;

  /* Determine scan size */
  scan_size_bits = jtag_scan_size(cmd->cmd.scan);
  scan_size_bytes = DIV_ROUND_UP(scan_size_bits, 8);

  /* Determine scan type (IN/OUT/IO) */
  type = jtag_scan_type(cmd->cmd.scan);

  /* Determine number of scan commands with maximum payload */
  scans_max_payload = scan_size_bytes / 58;

  /* Determine size of last shift command */
  bits_last_scan = scan_size_bits - (scans_max_payload * 58 * 8);

  /* Allocate TDO buffer if required */
  if ((type == SCAN_IN) || (type == SCAN_IO)) {
    tdo_buffer_start = calloc(sizeof(uint8_t), scan_size_bytes);

    if (tdo_buffer_start == NULL) {
      return ERROR_FAIL;
    }

    tdo_buffer = tdo_buffer_start;
  }

  /* Fill TDI buffer if required */
  if ((type == SCAN_OUT) || (type == SCAN_IO)) {
    jtag_build_buffer(cmd->cmd.scan, &tdi_buffer_start);
    tdi_buffer = tdi_buffer_start;
  }

  /* Get TAP state transitions */
  if (cmd->cmd.scan->ir_scan) {
    ulink_set_end_state(TAP_IRSHIFT);
    first_tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());
    first_tms_sequence = tap_get_tms_path(tap_get_state(), tap_get_end_state());

    tap_set_state(TAP_IRSHIFT);
    tap_set_end_state(cmd->cmd.scan->end_state);
    last_tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());
    last_tms_sequence = tap_get_tms_path(tap_get_state(), tap_get_end_state());

    /* TAP state transitions for split scans */
    tms_count_pause = tap_get_tms_path_len(TAP_IRSHIFT, TAP_IRPAUSE);
    tms_sequence_pause = tap_get_tms_path(TAP_IRSHIFT, TAP_IRPAUSE);
    tms_count_resume = tap_get_tms_path_len(TAP_IRPAUSE, TAP_IRSHIFT);
    tms_sequence_resume = tap_get_tms_path(TAP_IRPAUSE, TAP_IRSHIFT);
  }
  else {
    ulink_set_end_state(TAP_DRSHIFT);
    first_tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());
    first_tms_sequence = tap_get_tms_path(tap_get_state(), tap_get_end_state());

    tap_set_state(TAP_DRSHIFT);
    tap_set_end_state(cmd->cmd.scan->end_state);
    last_tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());
    last_tms_sequence = tap_get_tms_path(tap_get_state(), tap_get_end_state());

    /* TAP state transitions for split scans */
    tms_count_pause = tap_get_tms_path_len(TAP_DRSHIFT, TAP_DRPAUSE);
    tms_sequence_pause = tap_get_tms_path(TAP_DRSHIFT, TAP_DRPAUSE);
    tms_count_resume = tap_get_tms_path_len(TAP_DRPAUSE, TAP_DRSHIFT);
    tms_sequence_resume = tap_get_tms_path(TAP_DRPAUSE, TAP_DRSHIFT);
  }

  /* Generate scan commands */
  bytecount = scan_size_bytes;
  while (bytecount > 0) {
    if (bytecount == scan_size_bytes) {
      /* This is the first scan */
      tms_count_start = first_tms_count;
      tms_sequence_start = first_tms_sequence;
    }
    else {
      /* Resume from previous scan */
      tms_count_start = tms_count_resume;
      tms_sequence_start = tms_sequence_resume;
    }

    if (bytecount > 58) { /* Full scan, at least one scan will follow */
      tms_count_end = tms_count_pause;
      tms_sequence_end = tms_sequence_pause;

      ret = ulink_append_scan_cmd(device, type, 58 * 8, tdi_buffer,
          tdo_buffer_start, tdo_buffer, tms_count_start, tms_sequence_start,
          tms_count_end, tms_sequence_end, cmd, false);

      bytecount -= 58;

      /* Update TDI and TDO buffer pointers */
      if (tdi_buffer_start != NULL) {
        tdi_buffer += 58;
      }
      if (tdo_buffer_start != NULL) {
        tdo_buffer += 58;
      }
    }
    else if (bytecount == 58) { /* Full scan, no further scans */
      tms_count_end = last_tms_count;
      tms_sequence_end = last_tms_sequence;

      ret = ulink_append_scan_cmd(device, type, 58 * 8, tdi_buffer,
          tdo_buffer_start, tdo_buffer, tms_count_start, tms_sequence_start,
          tms_count_end, tms_sequence_end, cmd, true);

      bytecount = 0;
    }
    else { /* Scan with less than maximum payload, no further scans */
      tms_count_end = last_tms_count;
      tms_sequence_end = last_tms_sequence;

      ret = ulink_append_scan_cmd(device, type, bits_last_scan, tdi_buffer,
          tdo_buffer_start, tdo_buffer, tms_count_start, tms_sequence_start,
          tms_count_end, tms_sequence_end, cmd, true);

      bytecount = 0;
    }

    if (ret != ERROR_OK) {
      free(tdi_buffer_start);
      return ret;
    }
  }

  free(tdi_buffer_start);

  /* Set current state to the end state requested by the command */
  tap_set_state(cmd->cmd.scan->end_state);

  return ERROR_OK;
}

/**
 * Move the TAP into the Test Logic Reset state.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_tlr_reset(struct ulink *device, struct jtag_command *cmd)
{
  int ret;

  ret = ulink_append_clock_tms_cmd(device, 5, 0xff);

  if (ret == ERROR_OK) {
    tap_set_state(TAP_RESET);
  }

  return ret;
}

/**
 * Run Test.
 *
 * Generate TCK clock cycles while remaining
 * in the Run-Test/Idle state.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_runtest(struct ulink *device, struct jtag_command *cmd)
{
  int ret;

  /* Only perform statemove if the TAP currently isn't in the TAP_IDLE state */
  if (tap_get_state() != TAP_IDLE) {
    ulink_set_end_state(TAP_IDLE);
    ulink_queue_statemove(device);
  }

  /* Generate the clock cycles */
  ret = ulink_append_clock_tck_cmd(device, cmd->cmd.runtest->num_cycles);
  if (ret != ERROR_OK) {
    return ret;
  }

  /* Move to end state specified in command */
  if (cmd->cmd.runtest->end_state != tap_get_state()) {
    tap_set_end_state(cmd->cmd.runtest->end_state);
    ulink_queue_statemove(device);
  }

  return ERROR_OK;
}

/**
 * Execute a JTAG_RESET command
 *
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_reset(struct ulink *device, struct jtag_command *cmd)
{
  uint8_t low = 0, high = 0;

  if (cmd->cmd.reset->trst) {
    tap_set_state(TAP_RESET);
    high |= SIGNAL_TRST;
  }
  else {
    low |= SIGNAL_TRST;
  }

  if (cmd->cmd.reset->srst) {
    high |= SIGNAL_RESET;
  }
  else {
    low |= SIGNAL_RESET;
  }

  return ulink_append_set_signals_cmd(device, low, high);
}

/**
 * Move to one TAP state or several states in succession.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_pathmove(struct ulink *device, struct jtag_command *cmd)
{
  // TODO: Implement this!
  return ERROR_OK;
}

/**
 * Sleep for a specific amount of time.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @param cmd pointer to the command that shall be executed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_queue_sleep(struct ulink *device, struct jtag_command *cmd)
{
  /* IMPORTANT! Due to the time offset in command execution introduced by
   * command queueing, this needs to be implemented in the ULINK device */
  return ulink_append_sleep_cmd(device, cmd->cmd.sleep->us);
}

/**
 * Post-process JTAG_SCAN command
 *
 * @param ulink_cmd pointer to OpenULINK command that shall be processed.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_post_process_scan(ulink_cmd_t *ulink_cmd)
{
  struct jtag_command *cmd = ulink_cmd->cmd_origin;
  int ret;

  switch (jtag_scan_type(cmd->cmd.scan)) {
  case SCAN_IN:
  case SCAN_IO:
    ret = jtag_read_buffer(ulink_cmd->payload_in_start, cmd->cmd.scan);
    break;
  case SCAN_OUT:
    /* Nothing to do for OUT scans */
    ret = ERROR_OK;
    break;
  default:
    LOG_ERROR("BUG: ulink_post_process_scan() encountered an unknown"
        " JTAG scan type");
    ret = ERROR_FAIL;
    break;
  }

  return ret;
}

/**
 * Perform post-processing of commands after OpenULINK queue has been executed.
 *
 * @param device pointer to struct ulink identifying ULINK driver instance.
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
int ulink_post_process_queue(struct ulink *device)
{
  ulink_cmd_t *current;
  struct jtag_command *openocd_cmd;
  int ret;

  current = device->queue_start;

  while (current != NULL) {
    openocd_cmd = current->cmd_origin;

    /* Check if a corresponding OpenOCD command is stored for this
     * OpenULINK command */
    if ((current->needs_postprocessing == true) && (openocd_cmd != NULL)) {
      switch (openocd_cmd->type) {
      case JTAG_SCAN:
        ret = ulink_post_process_scan(current);
        break;
      case JTAG_TLR_RESET:
      case JTAG_RUNTEST:
      case JTAG_RESET:
      case JTAG_PATHMOVE:
      case JTAG_SLEEP:
        /* Nothing to do for these commands */
        ret = ERROR_OK;
        break;
      default:
        ret = ERROR_FAIL;
        LOG_ERROR("BUG: ulink_post_process_queue() encountered unknown JTAG "
            "command type");
        break;
      }

      if (ret != ERROR_OK) {
        return ret;
      }
    }

    current = current->next;
  }

  return ERROR_OK;
}

/**************************** JTAG driver functions ***************************/

/**
 * Executes the JTAG Command Queue.
 *
 * This is done in three stages: First, all OpenOCD commands are processed into
 * queued OpenULINK commands. Next, the OpenULINK command queue is sent to the
 * ULINK device and data received from the ULINK device is cached. Finally,
 * the post-processing function writes back data to the corresponding OpenOCD
 * commands.
 *
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
static int ulink_execute_queue(void)
{
  struct jtag_command *cmd = jtag_command_queue;
  int ret;

  while (cmd) {
    switch (cmd->type) {
    case JTAG_SCAN:
      ret = ulink_queue_scan(ulink_handle, cmd);
      break;
    case JTAG_TLR_RESET:
      ret = ulink_queue_tlr_reset(ulink_handle, cmd);
      break;
    case JTAG_RUNTEST:
      ret = ulink_queue_runtest(ulink_handle, cmd);
      break;
    case JTAG_RESET:
      ret = ulink_queue_reset(ulink_handle, cmd);
      break;
    case JTAG_PATHMOVE:
      ret = ulink_queue_pathmove(ulink_handle, cmd);
      break;
    case JTAG_SLEEP:
      ret = ulink_queue_sleep(ulink_handle, cmd);
      break;
    default:
      ret = ERROR_FAIL;
      LOG_ERROR("BUG: encountered unknown JTAG command type");
      break;
    }

    if (ret != ERROR_OK) {
      return ret;
    }

    cmd = cmd->next;
  }

  if (ulink_handle->commands_in_queue > 0) {
    ret = ulink_execute_queued_commands(ulink_handle, USB_TIMEOUT);
    if (ret != ERROR_OK) {
      return ret;
    }

    ret = ulink_post_process_queue(ulink_handle);
    if (ret != ERROR_OK) {
      return ret;
    }

    ulink_clear_queue(ulink_handle);
  }

  return ERROR_OK;
}

/**
 * Set the TCK frequency of the ULINK adapter.
 *
 * @param khz ???
 * @param jtag_speed ???
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
static int ulink_khz(int khz, int *jtag_speed)
{
  if (khz == 0) {
    LOG_ERROR("RCLK not supported");
    return ERROR_FAIL;
  }

  LOG_INFO("ulink_khz: %i kHz", khz);

  /* ULINK maximum TCK frequency is ~ 150 kHz */
  if (khz > 150) {
    return ERROR_FAIL;
  }

  *jtag_speed = 0;

  return ERROR_OK;
}

/**
 * Set the TCK frequency of the ULINK adapter.
 *
 * @param speed ???
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
static int ulink_speed(int speed)
{
  return ERROR_OK;
}

/**
 *
 */
static int ulink_speed_div(int speed, int *khz)
{
  LOG_INFO("ulink_speed_div: %i", speed);

  switch (speed) {
  case 0:
    *khz = 150;
    break;
  case 1:
    *khz = 100;
    break;
  }

  return ERROR_OK;
}

/**
 * Initiates the firmware download to the ULINK adapter and prepares
 * the USB handle.
 *
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
static int ulink_init(void)
{
  int ret;
  char str_manufacturer[20];
  bool download_firmware = false;
  uint8_t *dummy;
  uint8_t input_signals, output_signals;

  ulink_handle = calloc(1, sizeof(struct ulink));
  if (ulink_handle == NULL) {
    return ERROR_FAIL;
  }

  usb_init();

  ret = ulink_usb_open(&ulink_handle);
  if (ret != ERROR_OK) {
    LOG_ERROR("Could not open ULINK device");
    return ret;
  }

  /* Get String Descriptor to determine if firmware needs to be loaded */
  ret = usb_get_string_simple(ulink_handle->usb_handle, 1, str_manufacturer, 20);
  if (ret < 0) {
    /* Could not get descriptor -> Unconfigured or original Keil firmware */
    download_firmware = true;
  }
  else {
    /* We got a String Descriptor, check if it is the correct one */
    if (strncmp(str_manufacturer, "OpenULINK", 9) != 0) {
      download_firmware = true;
    }
  }

  if (download_firmware == true) {
    LOG_INFO("Loading OpenULINK firmware. This is reversible by power-cycling"
        " ULINK device.");
    ret = ulink_load_firmware_and_renumerate(&ulink_handle,
        ULINK_FIRMWARE_FILE, ULINK_RENUMERATION_DELAY);
    if (ret != ERROR_OK) {
      LOG_ERROR("Could not download firmware and re-numerate ULINK");
      return ret;
    }
  }
  else {
    LOG_INFO("ULINK device is already running OpenULINK firmware");
  }

  /* Initialize OpenULINK command queue */
  ulink_clear_queue(ulink_handle);

  /* Issue one test command with short timeout */
  ret = ulink_append_test_cmd(ulink_handle);
  if (ret != ERROR_OK) {
    return ret;
  }

  ret = ulink_execute_queued_commands(ulink_handle, 200);
  if (ret != ERROR_OK) {
    /* Sending test command failed. The ULINK device may be forever waiting for
     * the host to fetch an USB Bulk IN packet (e. g. OpenOCD crashed or was
     * shut down by the user via Ctrl-C. Try to retrieve this Bulk IN packet. */
    dummy = calloc(64, sizeof(uint8_t));

    ret = usb_bulk_read(ulink_handle->usb_handle, (2 | USB_ENDPOINT_IN),
        (char *)dummy, 64, 200);

    free(dummy);

    if (ret < 0) {
      /* Bulk IN transfer failed -> unrecoverable error condition */
      LOG_ERROR("Cannot communicate with ULINK device. Disconnect ULINK from "
          "the USB port and re-connect, then re-run OpenOCD");
      return ERROR_FAIL;
    }
#ifdef _DEBUG_USB_COMMS_
    else {
      /* Successfully received Bulk IN packet -> continue */
      LOG_INFO("Recovered from lost Bulk IN packet");
    }
#endif
  }
  ulink_clear_queue(ulink_handle);

  ulink_append_get_signals_cmd(ulink_handle);
  ulink_execute_queued_commands(ulink_handle, 200);

  /* Post-process the single CMD_GET_SIGNALS command */
  input_signals = ulink_handle->queue_start->payload_in[0];
  output_signals = ulink_handle->queue_start->payload_in[1];

  ulink_print_signal_states(input_signals, output_signals);

  ulink_clear_queue(ulink_handle);

  return ERROR_OK;
}

/**
 * Closes the USB handle for the ULINK device.
 *
 * @return on success: ERROR_OK
 * @return on failure: ERROR_FAIL
 */
static int ulink_quit(void)
{
  int ret;

  ret = ulink_usb_close(&ulink_handle);
  free(ulink_handle);

  return ret;
}

/*************************** Command Registration **************************/

struct jtag_interface ulink_interface = {
  .name = "ulink",
  .transports = jtag_only,

  .execute_queue = ulink_execute_queue,
  .khz = ulink_khz,
  .speed = ulink_speed,
  .speed_div = ulink_speed_div,

  .init = ulink_init,
  .quit = ulink_quit
};
