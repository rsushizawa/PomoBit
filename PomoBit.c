#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include <math.h>

// Biblioteca gerada pelo arquivo .pio durante compilação.
#include "ws2818b.pio.h"

// Define GPIO pins
#define BUTTON_STATE_PIN 5
#define BUTTON_PAUSE_PIN 6
#define STATUS_LED_PIN 13

// Definição do número de LEDs e pino.
#define LED_COUNT 25
#define LED_PIN 7

// Define time intervals (em segundos)
#define STUDY_TIME (25 * 60)  // Ex.: 12 segundos para teste
#define REST_TIME  (5 * 60)  // Ex.: 12 segundos para teste

// State machine states
typedef enum {
    STATE_STUDY,
    STATE_REST,
    STATE_PAUSED
} State;

// Definição de pixel GRB
struct pixel_t {
  uint8_t G, R, B; // Três valores de 8-bits compõem um pixel.
};
typedef struct pixel_t pixel_t;
typedef pixel_t npLED_t; // Mudança de nome de "struct pixel_t" para "npLED_t" por clareza.

// Declaração do buffer de pixels que formam a matriz.
npLED_t leds[LED_COUNT];

// Variáveis para uso da máquina PIO.
PIO np_pio;
uint sm;

// Global variables
State current_state = STATE_STUDY;
State previous_state = STATE_STUDY;  // Guarda o estado anterior para retomar após a pausa
int remaining_time = STUDY_TIME;
bool paused = false;

// Variáveis para debounce (armazenam o último estado lido dos botões)
bool last_state_button_state = true; // true = não pressionado (pull-up ativo)
bool last_state_button_pause = true;

// Para controle do piscar do LED no estado PAUSED
bool led_on = false;
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

/**
 * Inicializa a máquina PIO para controle da matriz de LEDs.
 */
void npInit(uint pin) {

  // Cria programa PIO.
  uint offset = pio_add_program(pio0, &ws2818b_program);
  np_pio = pio0;

  // Toma posse de uma máquina PIO.
  sm = pio_claim_unused_sm(np_pio, false);
  if (sm < 0) {
    np_pio = pio1;
    sm = pio_claim_unused_sm(np_pio, true); // Se nenhuma máquina estiver livre, panic!
  }

  // Inicia programa na máquina PIO obtida.
  ws2818b_program_init(np_pio, sm, offset, pin, 800000.f);

  // Limpa buffer de pixels.
  for (uint i = 0; i < LED_COUNT; ++i) {
    leds[i].R = 0;
    leds[i].G = 0;
    leds[i].B = 0;
  }
}

/**
 * Atribui uma cor RGB a um LED.
 */
void npSetLED(const uint index, const uint8_t r, const uint8_t g, const uint8_t b) {
  leds[index].R = r;
  leds[index].G = g;
  leds[index].B = b;
}

/**
 * Limpa o buffer de pixels.
 */
void npClear() {
  for (uint i = 0; i < LED_COUNT; ++i)
    npSetLED(i, 0, 0, 0);
}

/**
 * Escreve os dados do buffer nos LEDs.
 */
void npWrite() {
  // Escreve cada dado de 8-bits dos pixels em sequência no buffer da máquina PIO.
  for (uint i = 0; i < LED_COUNT; ++i) {
    pio_sm_put_blocking(np_pio, sm, leds[i].G);
    pio_sm_put_blocking(np_pio, sm, leds[i].R);
    pio_sm_put_blocking(np_pio, sm, leds[i].B);
  }
  sleep_us(100); // Espera 100us, sinal de RESET do datasheet.
}

/**
 * Indica o stado do progama no STATUS_LED_PIN
 */
void update_status_led(absolute_time_t now) {
    if (current_state == STATE_STUDY) {
        gpio_put(STATUS_LED_PIN, 1);
    } else if (current_state == STATE_REST) {
        gpio_put(STATUS_LED_PIN, 0);
    } else if (current_state == STATE_PAUSED) {
        // Se passaram 500ms desde a última troca, inverte o LED.
        if (absolute_time_diff_us(last_led_toggle_time, now) >= 500 * 1000) {
            led_on = !led_on;
            gpio_put(STATUS_LED_PIN, led_on);
            last_led_toggle_time = now;
        }
    }
}

/**
 * Handler do tempo de cada estado
 */
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

/**
 * Processa o input dos botões
 */
void process_buttons() {
    // Lê os estados atuais (botões ativos em nível baixo)
    bool current_button_state = gpio_get(BUTTON_STATE_PIN);
    bool current_button_pause = gpio_get(BUTTON_PAUSE_PIN);

    // Detecta borda de descida para o botão de mudança de estado
    // (transição de 1 para 0 indica que o botão foi pressionado)
    if (last_state_button_state && !current_button_state) {
        // Botão pressionado: alterna entre study e rest.
        // Se estiver pausado, retoma o timer e alterna para o outro estado
        if (paused) {
            // Ignora a troca de estado se estiver pausado
        } else {
            if (current_state == STATE_STUDY) {
                current_state = STATE_REST;
                remaining_time = REST_TIME;
            } else {
                current_state = STATE_STUDY;
                remaining_time = STUDY_TIME;
            }
        }
    }
    last_state_button_state = current_button_state;

    // Detecta borda de descida para o botão de pausa
    if (last_state_button_pause && !current_button_pause) {
        // Se não estiver pausado, entra no modo pausa e salva o estado atual
        if (!paused) {
            paused = true;
            previous_state = current_state; // Salva o estado para retomada
            current_state = STATE_PAUSED;    // Muda para modo pausado (para controle do LED)
            led_on = false;
            last_led_toggle_time = get_absolute_time();
        } else {
            // Se estiver pausado, retoma o timer restaurando o estado anterior
            paused = false;
            current_state = previous_state;
        }
    }
    last_state_button_pause = current_button_pause;
}


void led_matrix_visual(){
    int leds_active = ceil(remaining_time / 60);
    for(int i = 0; i < leds_active; i++){
        npSetLED(i,255,255,255);
    }
    npWrite(); // Escreve os dados nos LEDs.
    npClear();
}

int main()
{
    stdio_init_all();
    initialize_gpio();

    // Inicializa matriz de LEDs NeoPixel.
    npInit(LED_PIN);
    npClear();

    // Inicializa os tempos de referência
    absolute_time_t last_tick_time = get_absolute_time();
    last_led_toggle_time = get_absolute_time();


    npWrite(); // Escreve os dados nos LEDs.
    

    while (true) {
        absolute_time_t now = get_absolute_time();

        // Processa os botões (detecção de borda para não bloquear)
        process_buttons();

        // Atualiza o timer do pomodoro
        update_timer(now, &last_tick_time);

        // Atualiza o LED de status de acordo com o estado
        update_status_led(now);

        led_matrix_visual();

        // Pequena espera para evitar uso excessivo da CPU (10 ms)
        sleep_ms(10);
    }
    
    return 0;
}
