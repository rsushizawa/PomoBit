#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include <math.h>

// Biblioteca gerada pelo arquivo .pio durante compilação.
#include "ws2818b.pio.h"

/**
 * DEFINIÇÕES DE PINOS
 */
#define BUTTON_STATE_PIN    5   // Botão para trocar entre study/rest
#define BUTTON_PAUSE_PIN    6  // Botão para pausar o timer
#define STUDY_LED_PIN      11   // LED de status
#define REST_LED_PIN      13   // LED de status
#define CONFIG_LED_PIN      12   // config status

// Pinos do joystick:
#define JOYSTICK_ADC_PIN    26   // Usaremos este pino para o eixo (ex.: eixo vertical)
#define JOYSTICK_SWITCH_PIN 22   // Botão (switch) do joystick

// Intervalos mínimos e máximos (em minutos) para estudo/pausa
#define MIN_TIME_MINUTES 1
#define MAX_TIME_MINUTES 50

/**
 * DEFINIÇÃO DOS ESTADOS
 */
typedef enum {
    STATE_STUDY,
    STATE_REST,
    STATE_PAUSED,
    STATE_CONFIG    // Modo de configuração (ajuste de tempos via joystick)
} State;

/**
 * VARIÁVEIS GLOBAIS
 */

// Estados e timer
State current_state = STATE_STUDY;
State previous_state = STATE_STUDY;  // Para retomar após pausa ou configuração
int remaining_time = 0;  // em segundos

// Tempos configuráveis (em minutos)
int study_time_minutes = 25;  // valor padrão – pode ser ajustado via joystick
int rest_time_minutes  = 5;   // valor padrão

// Versões em segundos (calculadas sempre que os tempos são atualizados)
int study_duration = 25 * 60;
int rest_duration  = 5 * 60;

// Variáveis para debounce dos botões físicos
bool last_state_button_state = true; // true = não pressionado (pull-up ativo)
bool last_state_button_pause = true;

// Para controle do piscar do LED no estado PAUSED
bool led_on = false;
absolute_time_t last_led_toggle_time;

// Variáveis para o joystick switch (botão do joystick)
bool last_joystick_switch_state = true; // true = não pressionado
absolute_time_t joystick_switch_press_time;
bool joystick_long_press_handled = false;

// Para definir qual tempo está sendo editado no modo CONFIG
// Se true, o tempo de estudo está sendo ajustado; se false, o tempo de pausa.
bool editing_study = true;

// Variável para o último tick do timer
absolute_time_t last_tick_time;

// Definição do número de LEDs e pino.
#define LED_COUNT 25
#define LED_PIN 7

// Definição de pixel GRB
struct pixel_t {
    uint8_t G, R, B; // Três valores de 8-bits compõem um pixel.
};

typedef struct pixel_t pixel_t;
typedef pixel_t npLED_t; // Mudança de nome de "struct pixel_t" para "npLED_t" por clareza.

// Declaração do buffer de pixels que formam a matriz.
npLED_t leds[LED_COUNT];

// Variáveis para uso da máquina 'PIO.
PIO np_pio;
uint sm;

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
 * FUNÇÕES DE INICIALIZAÇÃO
 */
void initialize_gpio() {
    // Botões físicos:
    gpio_init(BUTTON_STATE_PIN);
    gpio_set_dir(BUTTON_STATE_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_STATE_PIN);
    
    gpio_init(BUTTON_PAUSE_PIN);
    gpio_set_dir(BUTTON_PAUSE_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PAUSE_PIN);
    
    // LED STUDY:
    gpio_init(STUDY_LED_PIN);
    gpio_set_dir(STUDY_LED_PIN, GPIO_OUT);

    // LED RES:
    gpio_init(REST_LED_PIN);
    gpio_set_dir(REST_LED_PIN, GPIO_OUT);

    // LED de config:
    gpio_init(CONFIG_LED_PIN);
    gpio_set_dir(CONFIG_LED_PIN, GPIO_OUT);
    
    // Joystick: switch
    gpio_init(JOYSTICK_SWITCH_PIN);
    gpio_set_dir(JOYSTICK_SWITCH_PIN, GPIO_IN);
    gpio_pull_up(JOYSTICK_SWITCH_PIN);
    
    // Joystick: eixo analógico
    adc_init();
    adc_gpio_init(JOYSTICK_ADC_PIN); // Configura o pino ADC para o joystick
}

/**
 * ATUALIZAÇÃO DO LED DE STATUS
 */
void update_status_led(absolute_time_t now) {
    if (current_state == STATE_PAUSED) {
        // Pisca a cada 500 ms
        if (absolute_time_diff_us(last_led_toggle_time, now) >= 500 * 1000) {
            led_on = !led_on;
            if (previous_state == STATE_REST){
                gpio_put(REST_LED_PIN, led_on);
            } else {
                if (previous_state == STATE_STUDY){
                    gpio_put(STUDY_LED_PIN, led_on);
                }
            }
            last_led_toggle_time = now;
        }
    }
    // Em STATE_CONFIG, o LED pode permanecer como estava ou ter outro padrão (opcional)
}

/**
 * Pisca a matriz de led
 * Usada para alertar o termino do periodo de estudo e descanso
 */
void flash_led_matrix(int nTimes,int delay_ms, int nLeds, int r, int g, int b){
    for (int i = 0; i < nTimes; i++) {
        for (int j = 0; j < LED_COUNT; j++) {
            npSetLED(j, r, g, b);
        }
        npWrite();
        sleep_ms(delay_ms);
        npClear();
        npWrite();
        sleep_ms(delay_ms);
    }
}

/**
 * PROCESSA OS BOTÕES FÍSICOS (study/rest e pause)
 * Estes botões só são processados em modo normal (não em configuração)
 */
void process_buttons() {
    // Se estivermos em configuração, ignoramos estes botões
    if (current_state == STATE_CONFIG) return;

    bool current_button_state = gpio_get(BUTTON_STATE_PIN);
    bool current_button_pause = gpio_get(BUTTON_PAUSE_PIN);

    // Detecta borda de descida para o botão de troca de estado
    if (last_state_button_state && !current_button_state) {
        // Se não estiver pausado, alterna entre study e rest
        if (current_state != STATE_PAUSED) {
            if (current_state == STATE_STUDY) {
                current_state = STATE_REST;
                remaining_time = rest_duration;
            } else {
                current_state = STATE_STUDY;
                remaining_time = study_duration;
            }
            previous_state = current_state;
        }
    }
    last_state_button_state = current_button_state;

    // Detecta borda de descida para o botão de pausa
    if (last_state_button_pause && !current_button_pause) {
        if (current_state != STATE_PAUSED) {
            // Pausa o timer e salva o estado atual
            previous_state = current_state;
            current_state = STATE_PAUSED;
        } else {
            // Se estava pausado, retoma o estado anterior
            current_state = previous_state;
        }
    }
    last_state_button_pause = current_button_pause;
}

/**
 * ATUALIZAÇÃO DO TIMER
 * Apenas atualiza quando não está em modo de configuração
 */
void update_timer(absolute_time_t now, absolute_time_t* last_tick_time) {
    if (current_state == STATE_STUDY || current_state == STATE_REST) {
        if (absolute_time_diff_us(*last_tick_time, now) >= 1000000) {  // 1 segundo
            if (remaining_time > 0) {
                remaining_time--;
            } else {
                // Quando chega a 0, troca de estado e reinicia o tempo
                if (current_state == STATE_STUDY) {
                    bool current_button_state = gpio_get(BUTTON_STATE_PIN);
                    while (current_button_state)
                    {
                        flash_led_matrix(1,200,25,64,64,0);
                        current_button_state = gpio_get(BUTTON_STATE_PIN);
                    }
                    npClear();
                    previous_state = current_state;
                    current_state = STATE_REST;
                    remaining_time = rest_duration;
                    sleep_ms(200);
                } else { // STATE_REST
                    bool current_button_state = gpio_get(BUTTON_STATE_PIN);
                    while (current_button_state)
                    {
                        flash_led_matrix(1,200,25,32,32,32);
                        current_button_state = gpio_get(BUTTON_STATE_PIN);
                    }
                    npClear();
                    previous_state = current_state;
                    current_state = STATE_STUDY;
                    remaining_time = study_duration;
                    sleep_ms(200);
                }
            }
            *last_tick_time = now;
        }
    }
}

/**
 * PROCESSA O SWITCH DO JOYSTICKs
 * Trata pressão curta e longa para entrar/ sair do modo CONFIG ou trocar o parâmetro a ser editado
 */
void process_joystick_button() {
    bool current_jsw = gpio_get(JOYSTICK_SWITCH_PIN);

    // Detecta borda de descida: botão acabou de ser pressionado
    if (last_joystick_switch_state && !current_jsw) {
        joystick_switch_press_time = get_absolute_time();
        joystick_long_press_handled = false;
    }
    
    // Enquanto estiver pressionado, verifica se já passou 2 segundos
    if (!current_jsw) {  // botão pressionado (nível baixo)
        if (!joystick_long_press_handled) {
            if (absolute_time_diff_us(joystick_switch_press_time, get_absolute_time()) > 2000000) {
                // Pressão longa detectada (2 s)
                gpio_put(CONFIG_LED_PIN, 0);
                joystick_long_press_handled = true;
                if (current_state == STATE_CONFIG) {
                    // Se já estiver em modo de configuração, sai dele (o timer fica pausado)
                    current_state = previous_state;
                } else {
                    if (current_state == STATE_PAUSED){
                        current_state == STATE_CONFIG;
                    } else{
                        // Se não estiver, entra no modo de configuração
                        previous_state = current_state;
                        current_state = STATE_CONFIG;
                    }
                    gpio_put(CONFIG_LED_PIN, 1);
                    gpio_put(STUDY_LED_PIN, 0);
                    gpio_put(REST_LED_PIN, 0);
                    // Ao entrar em CONFIG, é interessante pausar o timer para facilitar o ajuste:
                    // (aqui, você pode armazenar o tempo atual, se desejar)
                }
            }
        }
    }
    // Ao soltar o botão: se não foi longa, trata como pressão curta
    else if (!last_joystick_switch_state && current_jsw) {
        if (!joystick_long_press_handled) {
            // Pressão curta: se estivermos em CONFIG, alterna qual tempo será editado.
            if (current_state == STATE_CONFIG) {
                editing_study = !editing_study;
            }
        }
    }
    last_joystick_switch_state = current_jsw;
}

/**
 * ATUALIZA A CONFIGURAÇÃO USANDO O JOYSTICK (modo CONFIG)
 * Lê o ADC e mapeia para o intervalo [MIN_TIME_MINUTES, MAX_TIME_MINUTES]
 * O valor lido atualiza o tempo do parâmetro que está sendo editado.
 */
void update_joystick_config() {
    // Seleciona o canal correspondente (assumindo que JOYSTICK_ADC_PIN é ADC0)
    adc_select_input(0);
    uint adc_y_raw = adc_read();
    
    if (previous_state == STATE_STUDY) {
        if (adc_y_raw <= 150 && study_time_minutes > MIN_TIME_MINUTES){
            study_time_minutes--;
        }
        else{
            if (adc_y_raw >= 4000 && study_time_minutes < MAX_TIME_MINUTES){
                study_time_minutes++;
            }
        }
        study_duration = study_time_minutes * 60;
        remaining_time = study_duration;
    } else {
        if (previous_state == STATE_REST){
            if (adc_y_raw <= 150 && rest_time_minutes > MIN_TIME_MINUTES){
                rest_time_minutes--;
            }
            else{
                if (adc_y_raw >= 4000 && rest_time_minutes < MAX_TIME_MINUTES){
                    rest_time_minutes++;
                }
            }
            rest_duration = rest_time_minutes * 60;
            remaining_time = rest_duration;
        }
    }
}

/**
 * Liga 1 led para cada minuto 0 a 50
 */
void led_matrix_visual(int seconds, int r, int g, int b){
    float seconds_leds = seconds;
    int leds_active = ceil(seconds_leds / 60);

    if (leds_active > 25){
        for(int i = 0; i < 25; i++){
            npSetLED(i,r,g,b);
        }
        b += 16;
        for(int i = 0; i < leds_active-25; i++){
            npSetLED(i,r,g,b);
        }

    } else {
        for(int i = 0; i < leds_active; i++){
            npSetLED(i,r,g,b);
        }
    }

    npWrite(); // Escreve os dados nos LEDs.
    npClear();
}

/**
 * No mode de configuração atualiza os leds conforme o usuário muda os timers
 */
void update_matriz_config() {
    if (current_state == STATE_CONFIG){
        if (previous_state == STATE_STUDY){
            led_matrix_visual(study_duration, 0, 16, 0);
        }
        else {
            if (previous_state == STATE_REST){
                led_matrix_visual(rest_duration, 16, 0, 0);
            }
        }
    }
}

/**
 * FUNÇÃO MAIN
 */
int main() {
    stdio_init_all();
    initialize_gpio();

    // Inicializa matriz de LEDs NeoPixel.
    npInit(LED_PIN);
    npClear();
    
    remaining_time = study_duration;  // Começa em Study
    last_tick_time = get_absolute_time();
    last_led_toggle_time = get_absolute_time();

    while (true) {
        absolute_time_t now = get_absolute_time();
        
        // Processa o botão do joystick (sempre para detectar entrada de CONFIG)
        process_joystick_button();
        // Se estiver em modo de configuração, use o joystick para atualizar os tempos
        if (current_state == STATE_CONFIG) {
            update_joystick_config();
            update_matriz_config();
            sleep_ms(100);
        } else {

            
            // Modo normal: processa botões físicos, timer e LED
            process_buttons();
            update_timer(now, &last_tick_time);
            update_status_led(now);
            if (current_state == STATE_STUDY){
                led_matrix_visual(remaining_time, 0, 16, 0);
            } else if(current_state == STATE_REST){
                led_matrix_visual(remaining_time, 16, 0, 0);
            }

        }
        
        sleep_ms(100);
    }
    
    return 0;
}
