#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h> // something tells me its need to read the onboard temp sensor
#include <zephyr/sys/printk.h>  // for printing on console


#include <zephyr/bluetooth/bluetooth.h> //  for using bluetooth stack. switching BLE to ON  and advertising.   
// 
#include <zephyr/bluetooth/gatt.h>  // GATT table and everything that happens once a phone is connected.

#include <zephyr/bluetooth/uuid.h> // holds the IDs, meaning the names of services and characteristics.
#include <zephyr/sys/byteorder.h>  // byte order manipulation


static int16_t current_temp; // Units of 0.01 C per BLE spec (0x2A6E)
static bool notify_enabled;

/* Called when the phone/client toggles Notifications on or off */
static void temp_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* Called if the phone/client manually hits "Read" */
static ssize_t read_temp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &current_temp, sizeof(current_temp));
}
 

BT_GATT_SERVICE_DEFINE(ess_svc,

    // Service is like a folder, it holds a group of characterstics or files to be sent via BLE 
    BT_GATT_PRIMARY_SERVICE(BT_UUID_ESS),

    // This is like a file in a folder, i.e. temp is a file inside the folder(service)
    BT_GATT_CHARACTERISTIC(BT_UUID_TEMPERATURE,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_temp, NULL, &current_temp),
    
    // Client Characterstic Configuration - like on ON\OFF switch
    BT_GATT_CCC(temp_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // This is called Characterstic User Description(CUD) 
    // A very simple description of the Characterstic   
    BT_GATT_CUD("nRF52840-DK die temperature - Varun's remote sensing demo", BT_GATT_PERM_READ),
);



/* Advertising packet: Flags + 16-bit ESS UUID */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_ESS_VAL)),
};

/* Scan response packet: Device Name */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};





// Struct for onboard temp sensor on the NRF dk board 
const struct device *Temp_Sensor_Device_ptr = DEVICE_DT_GET_ANY(nordic_nrf_temp);

// struct for controlling the 4 onboaard LEDs on the board
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

int main(void)
{

        // a simple if statement which ORs 4 LED pin statuses, 
        // so if either of the LED is not ready to work, program exits with -1
        if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1) ||
        !gpio_is_ready_dt(&led2) || !gpio_is_ready_dt(&led3)) {
        return -1;
    }

    // condition to check if the temp sensor is ready and servicable 
    if (!Temp_Sensor_Device_ptr || !device_is_ready(Temp_Sensor_Device_ptr)) {
        printk("Temp sensor not ready\n");
        return -1;
    }


  // condition tocheck if BLuetooth is ready 
    if (bt_enable(NULL) != 0) {
        printk("Bluetooth init failed\n");
        return -1;
    }

    // bluetooth was ready but advertisment across 3 channels failed due to some reason   
    if (bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd)) != 0) {
        printk("Advertising failed to start\n");
        return -1;
    }

    // all OK, advertisment is progress
    printk("Advertising as %s\n", CONFIG_BT_DEVICE_NAME);


    struct sensor_value temp;


    // configureing the LEDs in Output mode starting OFF state
    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);

    // our word that rotates a 1 between the 4 LSBs of the word
    uint8_t state = 0b00000001;

    while (1) {
        
        if (sensor_sample_fetch(Temp_Sensor_Device_ptr) == 0 &&
            sensor_channel_get(Temp_Sensor_Device_ptr, SENSOR_CHAN_DIE_TEMP, &temp) == 0) {
            // val1 = integer part, val2 = millionths (divide by 10000 for 2 decimal places)
            printk("Die Temp: %d.%02d C\n", temp.val1, temp.val2 / 10000);

  

current_temp = sys_cpu_to_le16((int16_t)(temp.val1 * 100 + temp.val2 / 10000));

            if (notify_enabled) {
                bt_gatt_notify(NULL, &ess_svc.attrs[1], &current_temp, sizeof(current_temp));
            }
            
        } else {
            printk("Failed to read temp\n");
        }
 
 
        // we're extracting state for each of the 4 LEDs individually by left shifting 1 and ANDinf with a 0x1(we only need the LSB)  
        gpio_pin_set_dt(&led0, state & (1 << 0));
        gpio_pin_set_dt(&led1, state & (1 << 1));
        gpio_pin_set_dt(&led2, state & (1 << 2));
        gpio_pin_set_dt(&led3, state & (1 << 3));

         // left shift by 1 bit 
         // and if the result is greater than 0b00010000 then change the result to 0b00000001
        // this is done to turn one one LED at a time for the NRF's 4 rotating LEDs          
         state = (state < 0b00001000) ? (state << 1) : 0b00000001;
        

        k_msleep(200);
    }

    return 0;
}
