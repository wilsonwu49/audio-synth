#include "ee14lib.h"
#include <math.h> 

#define MAX_DAC 4095
#define NOTES_NUM 12
#define TABLE_SIZE 256
#define SAMPLE_RATE 48000
#define BUFFER_SIZE 256

int base_note = 440;
int step_count = 0;
int num_of_notes = 0;

// Whether or not the note is pressed
// C4, C#4 / Db4, D4, D#4 / Eb4, E4, F4, F#4 / Gb4, G4, G#4 / Ab4, A4, A#4 / Bb4, B4
bool notes[NOTES_NUM] = {0};
// Frequency of the note
// 261.63, 277.18, 293.66, 311.13, 329.63, 349.23, 369.99, 392.00, 415.30, 440.00, 466.16, 493.88
float note_freqs[NOTES_NUM];
// Phase Step of the note
// phase = (steps * phase step) % table size
// phase step = (freq * table size) / sample rate
float note_phase_step[NOTES_NUM];
// Phase accumulation
float note_phase[NOTES_NUM] = {0};

// Fully initializes full buffer to 0 to make sure double buffer method works
uint16_t buffer[BUFFER_SIZE] = {0};
// Fully saturated sampled sine table
uint16_t sin_table[TABLE_SIZE];

// Interrupt handler to deal with half and complete transfer for double buffer
void DMA1_Channel3_IRQHandler(void) {
    if(DMA1->ISR & DMA_ISR_HTIF3) {
        // Hit halfway point, about to play second half, generate new first half
        DMA1->IFCR |= DMA_IFCR_CHTIF3;
        generate_sample(0);
    }
    else if(DMA1->ISR & DMA_ISR_TCIF3) {
        // Hit end point, about to play first half, generate new second half
        DMA1->IFCR |= DMA_IFCR_CTCIF3;
        generate_sample(1);
    }
}

// Initialize the DAC for the STM32L432KC:
// Enables the DAC for use, configures a TIM6 trigger, 
// normal output mode, external pin with buffer
// Configured for DAC channel 1, uses PA4 or A3 on board
void dac_init (void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_DAC1EN;

    // Enable GPIO PA4 (A3)
    dac_gpio_init();

    // Disable DAC
    DAC1->CR &= ~DAC_CR_EN1;
    // DAC Channel 1 trigger enabled
    DAC1->CR |= DAC_CR_TEN1;
    // DAC Trigger Selection, TIM6_TRGO
    DAC1->CR &= ~DAC_CR_TSEL1_Msk;
    // Disable waveform generation, 
    DAC1->CR &= ~DAC_CR_WAVE1;

    // External for Speaker, Buffer to Amplify the Signal
    // Also, Enables normal mode for the DAC (0xx)
    // 000: DAC channel1 is connected to external pin with Buffer enabled
    DAC1->MCR &= ~DAC_MCR_MODE1;
    // Enables DAC Channel 1
    DAC1->CR |= DAC_CR_EN1;
}


// Initializes the DMA for DAC use, uses a 16 bit transfer for uint16_t of the
// sin table, uses memory to peripheral, goes to right holding register of DAC
// 100 number of data, Circular to allow for continuous noise production while
// car is running by enabling and disabling DMA
void dma_init (void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    DMA1_Channel3->CCR &= ~(DMA_CCR_EN_Msk);

    // Peripheral and Memory addresses
    DMA1_Channel3->CPAR = (uint32_t)&DAC1->DHR12R1;
    DMA1_Channel3->CMAR = (uint32_t)buffer;

    // Number of Data to Transfer
    DMA1_Channel3->CNDTR = 256;

    // Channel 3, CxS[3:0] - 0110 => DAC_CH1
    DMA1_CSELR->CSELR &= ~(DMA_CSELR_C3S_Msk);
    DMA1_CSELR->CSELR |= (0b0110 << DMA_CSELR_C3S_Pos);
    // Memory to Peripheral
    DMA1_Channel3->CCR |= DMA_CCR_DIR;
    // Circular
    DMA1_Channel3->CCR |= DMA_CCR_CIRC;
    // Memory Increment
    DMA1_Channel3->CCR |= DMA_CCR_MINC;

    // DMA Memory Size Transfer = 16 bits
    DMA1_Channel3->CCR &= ~(DMA_CCR_MSIZE_Msk);
    DMA1_Channel3->CCR |= DMA_CCR_MSIZE_0;
    DMA1_Channel3->CCR &= ~(DMA_CCR_PSIZE_Msk);
    DMA1_Channel3->CCR |= DMA_CCR_PSIZE_0;

    DMA1_Channel3->CCR |= DMA_CCR_HTIE;

    NVIC_EnableIRQ(DMA1_Channel3_IRQn);

    // Enable
    DMA1_Channel3->CCR |= DMA_CCR_EN;
}

// Intialize A3 as output pin for DAC
void dac_gpio_init (void) {
    // GPIO LOGIC
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    // Set PA4 (A3) to 11 for analog mode
    gpio_config_mode(A3, 0b11);
    // Disable Digital Logic
    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPD4_Msk;
}


// Initialize TIM6 for the DMA with sampling rate of 48000
void clock_init (void) {
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM6EN;

    int freq = SAMPLE_RATE;
    // TIM6 = CLK / (PSC + 1)*(ARR + 1) 
    uint16_t psc = (4000000 / (freq * 65535));
    TIM6->PSC = psc;
    TIM6->ARR = (4000000 / (freq * (psc + 1))) - 1;

    // Enable Update DMA Request, will generate a DMA request on update events
    TIM6->DIER |= TIM_DIER_UDE;

    // Enable TIM6 Master Mode - Update event is selected as TRGO
    TIM6->CR2 &= ~(TIM_CR2_MMS_Msk);
    TIM6->CR2 |= TIM_CR2_MMS_1;

    // Enable TIM6 Clock
    TIM6->CR1 |= TIM_CR1_CEN;
}

// Generate the samples for some half of the buffer based on what interrupt
// (half or complete)
void generate_sample (int buffer_number) {
    int start, end;
    if (buffer_number == 1) { // Loading second half of the buffer
        start = BUFFER_SIZE / 2;
        end = BUFFER_SIZE;
    } else { // Otherwise first half
        start = 0;
        end = BUFFER_SIZE / 2;
    }

    for(int i = start; i < end; i++){
        if (num_of_notes == 0) {
            buffer[i] = MAX_DAC / 2; // Silence/Off
            continue;
        }
        for(int j = 0; j < NOTES_NUM; j++){
            if (notes[j]) { // If the note is being played
                buffer[i] += sin_table[(int)note_phase[j]]; // Add sin value of certain note w/ its phase
                note_phase[j] += note_phase_step[j]; // Update phase by phase step
                if (note_phase[j] >= TABLE_SIZE) { // If phase exceeds limits of sin table, bring it back into range
                    note_phase[j] -= TABLE_SIZE;
                }
            }
        }
        if (num_of_notes > 1) buffer[i] /= num_of_notes; // Normalize the range of values for samples
    }
}

// Create a table for the sin function, sampling 100 times per period
void create_sin_table (void) {
    for(int i = 0; i < TABLE_SIZE; i++) { 
        uint16_t temp = MAX_DAC / 2 * (sin (3.14159 * 2 / TABLE_SIZE * i) + 1);
        sin_table[i] = temp;
    }
}

// Initialize the values for the tables for each note: 
// notes to be played, note frequencies, note phase steps
void init_tables (void) {
    for(int i = 0; i < NOTES_NUM; i++){
        notes[i] = 0;
        note_freqs[i] = base_note * pow(2, (i - 9) / NOTES_NUM);
        note_phase_step[i] = note_freqs[i] * TABLE_SIZE / SAMPLE_RATE;
    }
}