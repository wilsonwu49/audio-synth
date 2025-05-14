#include "ee14lib.h"

int main(){
    gpio_init();
    dac_init();
    dma_init();

    while (1) {}
}