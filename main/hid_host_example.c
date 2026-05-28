#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/usb_types_stack.h"
static usb_device_handle_t g_dev_hdl = NULL;
static usb_transfer_t *g_transfer = NULL;
static uint8_t g_intf = 0;
static uint8_t g_alt = 0;
static usb_host_client_handle_t client_hdl;

static void transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED)
    {
        printf("RAW: ");

        for (int i = 0; i < transfer->actual_num_bytes; i++)
        {
            printf("%02X ", transfer->data_buffer[i]);
        }

        printf("\n");

        usb_host_transfer_submit(transfer);
    }
}



static void client_event_cb(
    const usb_host_client_event_msg_t *event_msg,
    void *arg)
{
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        printf("NEW DEVICE addr=%d\n",
               event_msg->new_dev.address);

        usb_device_handle_t dev_hdl;

        esp_err_t err = usb_host_device_open(
            client_hdl,
            event_msg->new_dev.address,
            &dev_hdl);

        if (err != ESP_OK)
        {
            printf("OPEN FAILED: %s\n",
                   esp_err_to_name(err));
            return;
        }

        printf("DEVICE OPENED\n");

        g_dev_hdl = dev_hdl;

        const usb_config_desc_t *config_desc;

        err = usb_host_get_active_config_descriptor(
            dev_hdl,
            &config_desc);

        if (err != ESP_OK)
        {
            printf("GET CONFIG FAILED\n");
            return;
        }

        const uint8_t *p = config_desc->val;

        int offset = 0;

        const usb_intf_desc_t *found_intf = NULL;

        uint8_t ep_addr = 0;
        uint16_t packet_size = 8;

        while (offset < config_desc->wTotalLength)
        {
            usb_standard_desc_t *desc =
                (usb_standard_desc_t *)(p + offset);

            if (desc->bDescriptorType ==
                USB_B_DESCRIPTOR_TYPE_INTERFACE)
            {
                const usb_intf_desc_t *intf =
                    (const usb_intf_desc_t *)desc;

                printf("Interface class: %02X\n",
                       intf->bInterfaceClass);

                found_intf = intf;
            }

            if (desc->bDescriptorType ==
                USB_B_DESCRIPTOR_TYPE_ENDPOINT)
            {
                const usb_ep_desc_t *ep =
                    (const usb_ep_desc_t *)desc;

                if ((ep->bmAttributes &
                     USB_BM_ATTRIBUTES_XFERTYPE_MASK)
                    == USB_BM_ATTRIBUTES_XFER_INT)
                {
                    if (ep->bEndpointAddress & 0x80)
                    {
                        ep_addr = ep->bEndpointAddress;

                        packet_size = ep->wMaxPacketSize;

                        printf("Interrupt IN EP: 0x%02X size=%d\n",
                               ep_addr,
                               packet_size);

                        break;
                    }
                }
            }

            offset += desc->bLength;
        }

        if (ep_addr == 0 || found_intf == NULL)
        {
            printf("No valid endpoint found\n");
            return;
        }

        g_intf = found_intf->bInterfaceNumber;
        g_alt = found_intf->bAlternateSetting;

        err = usb_host_interface_claim(
            client_hdl,
            dev_hdl,
            g_intf,
            g_alt);

        if (err != ESP_OK)
        {
            printf("CLAIM FAILED: %s\n",
                   esp_err_to_name(err));
            return;
        }

        printf("INTERFACE CLAIMED\n");

        usb_transfer_t *transfer;

        err = usb_host_transfer_alloc(
            packet_size,
            0,
            &transfer);

        if (err != ESP_OK)
        {
            printf("TRANSFER ALLOC FAILED\n");
            return;
        }

        g_transfer = transfer;

        transfer->device_handle = dev_hdl;
        transfer->bEndpointAddress = ep_addr;
        transfer->callback = transfer_cb;
        transfer->num_bytes = packet_size;

        err = usb_host_transfer_submit(transfer);

        if (err != ESP_OK)
        {
            printf("SUBMIT FAILED: %s\n",
                   esp_err_to_name(err));
            return;
        }

        printf("Listening raw HID data...\n");
    }

    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
    {
        printf("DEVICE GONE\n");

        if (g_transfer)
        {
            usb_host_transfer_free(g_transfer);
            g_transfer = NULL;
        }

        if (g_dev_hdl)
        {
            usb_host_interface_release(
                client_hdl,
                g_dev_hdl,
                g_intf);

            usb_host_device_close(
                client_hdl,
                g_dev_hdl);

            g_dev_hdl = NULL;
        }

        printf("Cleanup done\n");
    }
}
static void usb_lib_task(void *arg)
{
    while (1)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(
            portMAX_DELAY,
            &event_flags);
    }
}

static void usb_client_task(void *arg)
{
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        }};

    usb_host_client_register(
        &client_config,
        &client_hdl);

    while (1)
    {
        usb_host_client_handle_events(
            client_hdl,
            portMAX_DELAY);
    }
}

void app_main(void)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    usb_host_install(&host_config);

    xTaskCreate(
        usb_lib_task,
        "usb_lib",
        4096,
        NULL,
        20,
        NULL);

    xTaskCreate(
        usb_client_task,
        "usb_client",
        8192,
        NULL,
        20,
        NULL);
}