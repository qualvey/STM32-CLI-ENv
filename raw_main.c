#define RCC_APB2ENR  (*(volatile unsigned int *)0x40021018)
#define GPIOC_CRH    (*(volatile unsigned int *)0x40011004)
#define GPIOC_ODR    (*(volatile unsigned int *)0x4001100C)

void delay(int n) {
    while(n--) { __asm("nop"); }
}
void SystemInit(void) {}
int main(void) {
    RCC_APB2ENR |= (1 << 4);    // 开启 GPIOC 时钟
    GPIOC_CRH &= ~(0xF << 20);  // 清空 PC13 配置
    GPIOC_CRH |= (0x3 << 20);   // 设置 PC13 为推挽输出

    while(1) {
        GPIOC_ODR ^= (1 << 13); // 翻转 PC13
        delay(500000);
    }
}
