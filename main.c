#include "stm32f1xx.h"

/**
 * @brief  启动文件 startup_stm32f103xb.s 会跳转到这个函数。
 * 在进入 main 之前，我们需要配置系统时钟。
 */
void SystemInit(void) {
  /* * 在这里通常会配置 HSI/HSE 时钟。
   * 对于简单的“点灯”练习，我们可以先留空，芯片会默认使用内部 8MHz 晶振运行。
   */
}

void EXTI0_IRQHandler(void) {
  // 检查是不是 EXTI0 触发的中断
  if (EXTI->PR & (1 << 0)) {
    // 执行你的逻辑：比如翻转 LED
    GPIOC->ODR ^= (1 << 13);

    // 【极其重要】清除中断标志位！
    // 如果不清除，CPU 会以为中断一直存在，卡死在里面
    EXTI->PR |= (1 << 0);
  }
}
/**
 * @brief  简单的延时函数
 */
void delay(uint32_t count) {
  for (volatile uint32_t i = 0; i < count; i++)
    ;
}

int main(void) {
  // 1. 开启 AFIO (复用功能) 时钟，因为中断映射属于复用功能
  RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
  // 2. 将 PA0 映射到 EXTI0 线
  // AFIO->EXTICR[0] 负责控制引脚 0-3
  AFIO->EXTICR[0] &= ~(0xF << 0); // 清零，默认 0000 就是 PA0
  // 3. 配置 EXTI 控制器
  EXTI->IMR |= (1 << 0);  // 允许线 0 的中断请求
  EXTI->FTSR |= (1 << 0); // 设置下降沿触发 (按键按下瞬间)
  // 4. 配置 NVIC (给中断“开绿灯”)
  // EXTI0 的中断通道号通常是 6
  NVIC_EnableIRQ(EXTI0_IRQn);

  /* 1. 开启 GPIOC A 的外设时钟 */
  /* F103 的 PC13 在 APB2 总线上 */
  RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

  /* 2. 配置 PC13 引脚模式 */
  /* CRH 是高 8 位引脚寄存器，每个引脚占 4 位。PC13 对应 [23:20] 位 */
  /* 先清除原有的配置，再设置为 0x3 (通用推挽输出，50MHz) */
  GPIOC->CRH &= ~(0xF << 20);
  GPIOC->CRH |= (0x3 << 20);

  GPIOA->CRL &= ~(0xF << 0); // 清空 PA0 的配置
  GPIOA->CRL |= (0x8 << 0);  // 设置为 1000 (带上/下拉输入)
                             //
  GPIOA->CRL &= ~(0xF << 24);
  GPIOA->CRL |= (0x3 << 24);
  /* 3. 设置初始状态（关键点！） */
  // 让 PC13 初始为低电平，PA6 初始为高电平
  GPIOC->ODR &= ~(1 << 13);
  // ~ 是取反操作
  // &是与运算, &=是运算并赋值
  // ODR(Output Data Register)
  // GPIOA->ODR |= (1 << 6);

  GPIOA->ODR |= (1 << 6);
  GPIOA->ODR |= (1 << 0); // PA0 设为上拉，防止引脚电平“悬空”乱跳

  while (1) {
    GPIOC->ODR ^= GPIO_ODR_ODR13;
    GPIOA->ODR ^= GPIO_ODR_ODR6;

    /* 4. 延时 */
    delay(140000);
  }
}
