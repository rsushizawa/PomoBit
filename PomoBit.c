#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

// Define GPIO pins
#define BUTTON_STATE_PIN 14
#define BUTTON_PAUSE_PIN 15
#define STATUS_LED_PIN 13

// Define time intervals (em segundos)
#define STUDY_TIME (0.2 * 60)  // Ex.: 12 segundos para teste
#define REST_TIME  (0.2 * 60)  // Ex.: 12 segundos para teste

// State machine states
typedef enum {
    STATE_STUDY,
    STATE_REST,
    STATE_PAUSED
} State;

// Global variables
State current_state = STATE_STUDY;
State previous_state = STATE_STUDY;  // Guarda o estado anterior para retomar após a pausa
int remaining_time = STUDY_TIME;
bool paused = false;

absolute_time_t last_led_toggle_time;

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

void update_timer(absolute_time_t now, absolute_time_t* last_tick_time) {
    if (!paused && (current_state == STATE_STUDY || current_state == STATE_REST)) {
        // Verifica se passou um segundo desde a última atualização
        if (absolute_time_diff_us(*last_tick_time, now) >= 1000000) {
            if (remaining_time > 0) {
                remaining_time--;
            } else {
                // Quando o timer chega a 0, inverte o estado e reinicia o tempo.
                if (current_state == STATE_STUDY) {
                    current_state = STATE_REST;
                    remaining_time = REST_TIME;
                } else { // STATE_REST
                    current_state = STATE_STUDY;
                    remaining_time = STUDY_TIME;
                }
                // Atualiza o estado anterior, pois estamos executando normalmente
                previous_state = current_state;
            }
            // Atualiza o tempo do último tick para compensar possíveis atrasos.
            *last_tick_time = now;
        }
    }
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
