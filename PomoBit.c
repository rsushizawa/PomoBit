#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

// Define GPIO pins
#define BUTTON_STATE_PIN 14
#define BUTTON_PAUSE_PIN 15
#define STATUS_LED_PIN 13

void initialize_gpio() {
    // Botão que muda o estado
    gpio_init(BUTTON_STATE_PIN);
    gpio_set_dir(BUTTON_STATE_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_STATE_PIN);
    
    // Botão que pausa o timer
    gpio_init(BUTTON_PAUSE_PIN);
    gpio_set_dir(BUTTON_PAUSE_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PAUSE_PIN);
    
    // LED de status
    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);
}

int main()
{
    stdio_init_all();
    initialize_gpio(); 

    while (true) {
        printf("Hello, world!\n");
        sleep_ms(1000);
    }
}
