#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/adc.h"

// Inclua aqui a biblioteca da sua OLED – este exemplo supõe uma interface simples:
#include "ssd1306.h"  // Certifique-se de ter essa biblioteca instalada e configurada

/**
 * DEFINIÇÕES DE PINOS
 */
#define BUTTON_STATE_PIN    5   // Botão para trocar entre study/rest
#define BUTTON_PAUSE_PIN    6  // Botão para pausar o timer
#define STATUS_LED_PIN      13   // LED de status

// Pinos do joystick:
#define JOYSTICK_ADC_PIN    26   // Usaremos este pino para o eixo (ex.: eixo vertical)
#define JOYSTICK_SWITCH_PIN 22   // Botão (switch) do joystick

// Pinos OLED:
const uint I2C_SDA = 14;
const uint I2C_SCL = 15;

// Intervalos mínimos e máximos (em minutos) para estudo/pausa
#define MIN_TIME_MINUTES 5
#define MAX_TIME_MINUTES 25

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
    
    // Joystick: switch
    gpio_init(JOYSTICK_SWITCH_PIN);
    gpio_set_dir(JOYSTICK_SWITCH_PIN, GPIO_IN);
    gpio_pull_up(JOYSTICK_SWITCH_PIN);
    
    // Joystick: eixo analógico
    adc_init();
    adc_gpio_init(JOYSTICK_ADC_PIN); // Configura o pino ADC para o joystick

    // Inicialização do i2c
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

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
                    current_state = STATE_PAUSED;
                } else {
                    // Se não estiver, entra no modo de configuração
                    previous_state = current_state;
                    current_state = STATE_CONFIG;
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
    uint16_t raw = adc_read();
    // Mapeia: 0 -> MIN_TIME_MINUTES e 4095 -> MAX_TIME_MINUTES
    int new_time = MIN_TIME_MINUTES + (raw * (MAX_TIME_MINUTES - MIN_TIME_MINUTES)) / 4095;
    
    if (editing_study) {
        study_time_minutes = new_time;
        study_duration = study_time_minutes * 60;
    } else {
        rest_time_minutes = new_time;
        rest_duration = rest_time_minutes * 60;
    }
}

/**
 * ATUALIZA A OLED EM MODO CONFIG (ajuste de tempos)
 */
void update_oled_config() {

    // Processo de inicialização completo do OLED SSD1306
    ssd1306_init();

    // Preparar área de renderização para o display (ssd1306_width pixels por ssd1306_n_pages páginas)
    struct render_area frame_area = {
        start_column : 0,
        end_column : ssd1306_width - 1,
        start_page : 0,
        end_page : ssd1306_n_pages - 1
    };

    calculate_render_area_buffer_length(&frame_area);

    // zera o display inteiro
    uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    restart:

    char *text[] = {
    "  CONFIG MODE   ",
    "  Embarcatech   "};

    int y = 0;
    for (uint i = 0; i < count_of(text); i++)
    {
        ssd1306_draw_string(ssd, 5, y, text[i]);
        y += 8;
    }
    render_on_display(ssd, &frame_area);
}

/**
 * FUNÇÃO MAIN
 */
int main() {
    stdio_init_all();
    initialize_gpio();
    
    // Inicializa a OLED (supondo que a função exista na sua biblioteca)
    ssd1306_init();
    
    // Inicializa tempos
    study_duration = study_time_minutes * 60;
    rest_duration  = rest_time_minutes * 60;
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
            update_oled_config();
        } else {
            // Modo normal: processa botões físicos, timer e LED
            process_buttons();
            update_timer(now, &last_tick_time);
            update_status_led(now);
        }
        
        sleep_ms(10);
    }
    
    return 0;
}
