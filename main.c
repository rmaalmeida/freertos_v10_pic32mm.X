
#include "mcc_generated_files/mcc.h"


#include "FreeRTOS.h"
#include "task.h"

enum {
    LED_STATE_OFF, LED_STATE_ON
};

#include "queue.h"

typedef struct {
    unsigned int num; // 1, 2
    unsigned int status; // LED_STATE_OFF, LED_STATE_ON
} LedInfo;

QueueHandle_t ledQueue;

void taskLeds(void *pvParameters) {
    LedInfo led;
    for (;;) {
        xQueueReceive(ledQueue, &led, portMAX_DELAY);
        if (led.num == 1) {
            if (led.status == LED_STATE_ON) {
                IO_RA0_SetHigh();
            } else {
                IO_RA0_SetLow();
            }
        }
        if (led.num == 2) {
            if (led.status == LED_STATE_ON) {
                IO_RC9_SetHigh();
            } else {
                IO_RC9_SetLow();
            }
        }
    }
}

void taskButtons(void *pvParameters) {
    LedInfo led;
    int num = 1;
    for (;;) {
        led.num = 1;
        if (!IO_RB7_GetValue()) {
            led.status = LED_STATE_ON;
        } else {
            led.status = LED_STATE_OFF;
        }
        xQueueSend(ledQueue, &led, portMAX_DELAY);

        led.num = 2;
        if (!IO_RB13_GetValue()) {
            led.status = LED_STATE_ON;
        } else {
            led.status = LED_STATE_OFF;
        }
        xQueueSend(ledQueue, &led, portMAX_DELAY);
    }
}


QueueHandle_t pwmQueue;

static void lerADC(void *pvParameters) {
    int i;
    unsigned int conversion = 0;
    ADC1_ChannelSelect(ADC1_CHANNEL_AN11);
    for (;;) {
        ADC1_Start();
        for (i = 0; i < 1000; i++);
        ADC1_Stop();
        while (!ADC1_IsConversionComplete());
        if (conversion != ADC1_ConversionResultGet()) {
            conversion = ADC1_ConversionResultGet();
            xQueueSend(pwmQueue, &conversion, portMAX_DELAY);
        } else {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
}

static void taskPwm(void *pvParameters) {
    float t;
    int readValor = 0;
    MCCP1_COMPARE_Start();
    for (;;) {
        xQueueReceive(pwmQueue, &readValor, portMAX_DELAY);
        MCCP1_COMPARE_CenterAlignedPWMConfig(0, readValor);
    }
}

#include "semphr.h"
SemaphoreHandle_t buttonSem;

void red2(void *pvParameters) {
    int antigo, novo = 0;
    for (;;) {
        novo = !IO_RB7_GetValue();
        if (antigo != novo) {
            antigo = novo;
            if (novo) {
                xSemaphoreTake(buttonSem, portMAX_DELAY);
                IO_RA3_SetHigh();
            } else {
                IO_RA3_SetLow();
                xSemaphoreGive(buttonSem);
            }
        }
    }
}

void green2(void *pvParameters) {
    int antigo, novo = 0;
    for (;;) {
        novo = !IO_RB13_GetValue();
        if (antigo != novo) {
            antigo = novo;
            if (novo) {
                xSemaphoreTake(buttonSem, portMAX_DELAY);
                IO_RB12_SetHigh();
            } else {
                IO_RB12_SetLow();
                xSemaphoreGive(buttonSem);
            }
        }
    }
}

#define f_kp  10.0
#define f_ki  50.0
#define f_kd   0.0
#define f_T    0.005
#define t_ticks 5
#define SHIFT (1<<SHIFT_N)
#define SHIFT_N  (8)

#define MAX_SAT 1023
#define MIN_SAT 0


const int32_t i_k1 = (f_kp + f_ki * f_T + f_kd / f_T) * SHIFT;
const int32_t i_k2 = -((f_kp + 2 * f_kd / f_T) * SHIFT);
const int32_t i_k3 = (f_kd / f_T) * SHIFT;


int16_t i_e0 = 0, i_e1 = 0, i_e2 = 0;
int16_t i_y0 = 0, i_y1 = 0;
int32_t sp;

int16_t pid_sw_fixed(int ad) {

    // Update variables
    i_y1 = i_y0;
    i_e2 = i_e1;
    i_e1 = i_e0;
    i_e0 = (sp - ad);

    // Processing
    //the multiplication is for a number range in 4.2(6 bits) (gains from zero up to +15.75) 
    // times 0.3ff (10 bits)
    //    y0 = y1 + (i_kp * (e0 - e2)) +
    //            (i_ki * (e0) * i_T) +
    //            (i_kd * (e0 - (2 * e1) + e2) / i_T);
    //stated in terms of errors instead of coefficients
    i_y0 = ((i_k1 * i_e0) + (i_k2 * i_e1) + (i_k3 * i_e2));
    i_y0 = i_y0 >> SHIFT_N;
    i_y0 += i_y1;

    // Saturation
    if (i_y0 > MAX_SAT) {
        i_y0 = MAX_SAT;
    } else if (i_y0 < MIN_SAT) {
        i_y0 = MIN_SAT;
    }

    return i_y0;
}

SemaphoreHandle_t adc_sem;

void sp_change(void *pvParameters) {
    int t;
    MCCP1_COMPARE_Start();
    

    for (;;) {
        //necessário semáforo para não interromper a leitura do AD no meio
        xSemaphoreTake(adc_sem, portMAX_DELAY);
        ADC1_ChannelSelect(ADC1_CHANNEL_AN11);
        ADC1_Start();
        for (t = 0; t < 1000; t++);
        ADC1_Stop();
        while (!ADC1_IsConversionComplete());
        sp = ADC1_ConversionResultGet();
        xSemaphoreGive(adc_sem);
        //fim leitura do AD

        MCCP1_COMPARE_DualCompareValueSet(0, sp>>2);

        vTaskDelay(10 / portTICK_PERIOD_MS);

    }
}

void task_PID(void *pvParameters) {
    uint16_t ad;
    uint16_t res;
    int t;
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = t_ticks / portTICK_PERIOD_MS;
    xLastWakeTime = xTaskGetTickCount();

    SCCP2_COMPARE_Start();
    for (;;) {
        //necessário semáforo para não interromper a leitura do AD no meio
        xSemaphoreTake(adc_sem, portMAX_DELAY);
        ADC1_ChannelSelect(ADC1_CHANNEL_AN4);
        ADC1_Start();
        for (t = 0; t < 1000; t++);
        ADC1_Stop();
        while (!ADC1_IsConversionComplete());
        ad = ADC1_ConversionResultGet();
        xSemaphoreGive(adc_sem);
        //fim leitura do AD

        //cálculo PID e atualização da saída
        res = pid_sw_fixed(ad);
        SCCP2_COMPARE_DualCompareValueSet(0, res>>2);

        //garantia de tempo real
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void main(void) {
    SYSTEM_Initialize();

    xTaskCreate(task_PID, "PID", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(sp_change, "sp", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);


    adc_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(adc_sem);

    /* Start the tasks and timer running. */
    vTaskStartScheduler();
}

/**
 End of File
 */