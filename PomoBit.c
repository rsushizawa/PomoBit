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
#include "hardware/pwm.h"
#include <math.h>

// Biblioteca gerada pelo arquivo .pio durante compilação.
#include "ws2818b.pio.h"

/**
 * DEFINIÇÕES DE PINOS
 */
#define BUTTON_STATE_PIN    5   // Botão para trocar entre study/rest
#define BUTTON_PAUSE_PIN    6  // Botão para pausar o timer
#define STATUS_LED_PIN      13   // LED de status
#define CONFIG_LED_PIN      12   // config status

// Pinos do joystick:
#define JOYSTICK_ADC_PIN    26   // Usaremos este pino para o eixo (ex.: eixo vertical)
#define JOYSTICK_SWITCH_PIN 22   // Botão (switch) do joystick

// Pinos OLED:
const uint I2C_SDA = 14;
const uint I2C_SCL = 15;

// Intervalos mínimos e máximos (em minutos) para estudo/pausa
#define MIN_TIME_MINUTES 5
#define MAX_TIME_MINUTES 25

// Define os pinos para o buzzer estéreo
#define BUZZER_LEFT_PIN  10
#define BUZZER_RIGHT_PIN 21

// Definições de notas musicais (frequências aproximadas em Hz)
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523

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
 * Função: play_tone
 * -----------------
 * Configura o PWM para gerar um tom de frequência 'frequency' durante
 * 'duration_ms' milissegundos, emitindo o sinal em ambos os pinos do buzzer.
 */
void play_tone(uint frequency, uint duration_ms) {
    // Escolhemos um divisor fixo para o PWM.
    // O cálculo da frequência é: frequency = 125e6 / (divider * (wrap+1))
    const float divider = 64.0f;

    // Como os dois pinos pertencem ao mesmo slice, usamos um deles para obter o número do slice.
    uint slice_num = pwm_gpio_to_slice_num(BUZZER_LEFT_PIN);

    // Calcula o valor de wrap para gerar a frequência desejada.
    // (wrap + 1) = 125e6 / (divider * frequency)
    uint32_t wrap = (uint32_t)(125000000 / (divider * frequency)) - 1;

    // Configura o divisor e o wrap do slice
    pwm_set_clkdiv(slice_num, divider);
    pwm_set_wrap(slice_num, wrap);

    // Configura os duty cycles para 50% (metade do valor de wrap) em ambos os canais,
    // gerando uma onda quadrada (tom puro)
    pwm_set_gpio_level(BUZZER_LEFT_PIN, wrap / 8);
    pwm_set_gpio_level(BUZZER_RIGHT_PIN, wrap / 8);

    // Ativa o PWM para iniciar o som
    pwm_set_enabled(slice_num, true);

    // Mantém o tom ativo pelo tempo desejado
    sleep_ms(duration_ms);

    // Desativa o PWM para silenciar o buzzer
    pwm_set_enabled(slice_num, false);

    // Pequena pausa entre os tons
    sleep_ms(50);
}

/**
 * FUNÇÃO de TOCAR UMA SEQUENCIA DE FREQUENCIAS/NOTAS
 */
void play_sound(int* melody, int* noteDurations, int n){
    for (int i = 0; i < n; i++) {
        play_tone(melody[i], noteDurations[i]);
    }
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
    
    // LED de status:
    gpio_init(STATUS_LED_PIN);
    gpio_set_dir(STATUS_LED_PIN, GPIO_OUT);

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

    // Configura os pinos do buzzer para a função PWM
    gpio_set_function(BUZZER_LEFT_PIN, GPIO_FUNC_PWM);
    gpio_set_function(BUZZER_RIGHT_PIN, GPIO_FUNC_PWM);

}

/**
 * ATUALIZAÇÃO DO LED DE STATUS
 */
void update_status_led(absolute_time_t now) {
    if (current_state == STATE_STUDY) {
        gpio_put(STATUS_LED_PIN, 1);
    } else if (current_state == STATE_REST) {
        gpio_put(STATUS_LED_PIN, 0);
    } else if (current_state == STATE_PAUSED) {
        // Pisca a cada 500 ms
        if (absolute_time_diff_us(last_led_toggle_time, now) >= 500 * 1000) {
            led_on = !led_on;
            gpio_put(STATUS_LED_PIN, led_on);
            last_led_toggle_time = now;
        }
    }
    // Em STATE_CONFIG, o LED pode permanecer como estava ou ter outro padrão (opcional)
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
                    current_state = STATE_REST;
                    remaining_time = rest_duration;
                } else { // STATE_REST
                    current_state = STATE_STUDY;
                    remaining_time = study_duration;
                }
                previous_state = current_state;
            }
            *last_tick_time = now;
        }
    }
}

/**
 * PROCESSA O SWITCH DO JOYSTICK
 * Trata pressão curta e longa para entrar/ sair do modo CONFIG ou trocar o parâmetro a ser editado
 */
void process_joystick_button() {
    bool current_jsw = gpio_get(JOYSTICK_SWITCH_PIN);
    int configMelody[] = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_E4 };
    int configNoteDurations[] = {150, 150, 150, 150};
    int configNotes = sizeof(configMelody) / sizeof(configMelody[0]);

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
                joystick_long_press_handled = true;
                if (current_state == STATE_CONFIG) {
                    // Se já estiver em modo de configuração, sai dele (o timer fica pausado)
                    current_state = previous_state;
                    gpio_put(CONFIG_LED_PIN, 0);
                    play_sound(configMelody, configNoteDurations,configNotes);
                } else {
                    // Se não estiver, entra no modo de configuração
                    previous_state = current_state;
                    current_state = STATE_CONFIG;
                    gpio_put(CONFIG_LED_PIN, 1);
                    gpio_put(STATUS_LED_PIN, 0);
                    play_sound(configMelody, configNoteDurations,configNotes);
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
    int configMelody[] = { NOTE_C4, NOTE_G4};
    int configNoteDurations[] = {100, 100};
    int configNotes = sizeof(configMelody) / sizeof(configMelody[0]);
    
    if (previous_state == STATE_STUDY) {
        if (adc_y_raw <= 150 && study_time_minutes > MIN_TIME_MINUTES){
            study_time_minutes--;
            play_sound(configMelody, configNoteDurations,configNotes);
        }
        else{
            if (adc_y_raw >= 4000 && study_time_minutes < MAX_TIME_MINUTES){
                study_time_minutes++;
                play_sound(configMelody, configNoteDurations,configNotes);
            }
        }
        study_duration = study_time_minutes * 60;
        remaining_time = study_duration;
    } else {
        if (previous_state == STATE_REST){
            if (adc_y_raw <= 150 && rest_time_minutes > MIN_TIME_MINUTES){
                rest_time_minutes--;
                play_sound(configMelody, configNoteDurations,configNotes);
            }
            else{
                if (adc_y_raw >= 4000 && rest_time_minutes < MAX_TIME_MINUTES){
                    rest_time_minutes++;
                    play_sound(configMelody, configNoteDurations,configNotes);
                }
            }
            rest_duration = rest_time_minutes * 60;
            remaining_time = rest_duration;
        }
    }
}

/**
 * LIGA 1 LED PARA CADA MINUTO 0 A 25
 */
void led_matrix_visual(int seconds, int r, int g, int b){
    float seconds_leds = seconds;
    int leds_active = ceil(seconds_leds / 60);
    for(int i = 0; i < leds_active; i++){
        npSetLED(i,r,g,b);
    }
    npWrite(); // Escreve os dados nos LEDs.
    npClear();
}

/**
 * NO MODO DE CONFIGURAÇÃO ATUALIZA OS TIMERS
 */
void update_matriz_config() {
    if (current_state == STATE_CONFIG){
        if (previous_state == STATE_STUDY){
            led_matrix_visual(study_duration, 0, 64, 0);
        }
        else {
            if (previous_state == STATE_REST){
                led_matrix_visual(rest_duration, 0, 0, 64);
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
    
    int startUpMelody[] = { NOTE_C4, NOTE_E4, NOTE_G4, NOTE_C5 };
    int startUpNoteDurations[] = { 200, 200, 200, 400 };
    int startUpNotes = sizeof(startUpMelody) / sizeof(startUpMelody[0]);
    play_sound(startUpMelody, startUpNoteDurations, startUpNotes);

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
            led_matrix_visual(remaining_time, 64, 64, 64);
        }
        
        sleep_ms(100);
    }
    
    return 0;
}
