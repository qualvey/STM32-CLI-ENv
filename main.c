#include "stm32f103xb.h"
#include "system_stm32f1xx.h"
#include <stdint.h>
#include <stdio.h>

// 定义一个简单的超时常量
#define I2C_TIMEOUT 10000
// 定义快速操作宏
#define SCL_H GPIOB->BSRR = GPIO_BSRR_BS8
#define SCL_L GPIOB->BSRR = GPIO_BSRR_BR8
#define SDA_H GPIOB->BSRR = GPIO_BSRR_BS9
#define SDA_L GPIOB->BSRR = GPIO_BSRR_BR9
#define SDA_READ (GPIOB->IDR & GPIO_IDR_IDR9)

#define MPU6050_ADDR 0xD0 // MPU6050 的 I2C 地址 (AD0 接地时)
//
volatile uint32_t ms_ticks = 0;
volatile uint8_t flag = 0;
volatile uint8_t fill_state = 0x00; // 初始状态为 0x00 (黑)
volatile uint32_t next_pc13 = 0;
volatile uint32_t next_pa6 = 0;
// pwm 调光，pc13
volatile uint8_t pwm_counter = 0;
volatile uint8_t brightness = 20;        // 0-100 级，数字越小越暗
volatile uint16_t long_press_timer = 0;  // 长按计时器
volatile uint16_t auto_repeat_timer = 0; // 连发频率计时器

#define LONG_PRESS_TIME 50 // 50 * 10ms = 500ms 进入长按
#define REPEAT_SPEED 10    // 10 * 10ms = 100ms 触发一次减法
//
typedef enum {
  KEY_STATE_IDLE,        // 空闲（松开）
  KEY_STATE_DEBOUNCE,    // 消抖（刚按下，观察中）
  KEY_STATE_PRESSED,     // 确认按下（已触发逻辑）
  KEY_STATE_WAIT_RELEASE // 等待松手
} KeyState_t;

volatile KeyState_t g_button_state = KEY_STATE_IDLE;
volatile KeyState_t g_buttona0_state = KEY_STATE_IDLE;
void SetClock72M(void) {
  RCC->CR |= (1 << 16); // 开启 HSE (外部晶振)
  while (!(RCC->CR & (1 << 17)))
    ; // 等待 HSE 稳定

  FLASH->ACR |= 0x02; // Flash 等待 2 个周期 (72M 必备)

  RCC->CFGR |= (0x07 << 18); // PLL 9 倍频 (8M * 9 = 72M)
  RCC->CFGR |= (1 << 16);    // PLL 选择 H0SE 为源

  RCC->CR |= (1 << 24); // 开启 PLL
  while (!(RCC->CR & (1 << 25)))
    ; // 等待 PLL 锁定

  RCC->CFGR |= 0x02; // 系统时钟切换到 PLL
  while ((RCC->CFGR & 0x0C) != 0x08)
    ; // 等待切换完成
}

void delay(uint32_t count) {
  for (volatile uint32_t i = 0; i < count; i++)
    ;
}
void SysTick_Handler(void) {
  ms_ticks++;

  // if (ms_ticks >= next_pc13) {
  //   GPIOC->ODR ^= GPIO_ODR_ODR13;

  //  // OLED_Clear(0xFF); // 所有的像素点都会被填满，屏幕变全白
  //  //
  //  next_pc13 = ms_ticks + 2000;
  //}

  // 检查是否到了 PA6 该闪烁的时间
  if (ms_ticks >= next_pa6) {
    GPIOA->ODR ^= GPIO_ODR_ODR6;
    next_pa6 = ms_ticks + 300;
  }
}
// B10,B11,Hard I2C2
void I2C2_Init(void) {
  // 1. 开启时钟
  RCC->APB2ENR |= RCC_APB2ENR_IOPBEN; // 开启 GPIOB 时钟
  RCC->APB1ENR |= RCC_APB1ENR_I2C2EN; // 开启 I2C2 时钟

  // 2. 配置 GPIO (PB10 -> SCL, PB11 -> SDA)
  // 目标：复用开漏输出 (AF_OD), 50MHz
  // 在 CRH 寄存器中，PB10 是 [11:8] 位，PB11 是 [15:12] 位
  // CNF=11 (复用开漏), MODE=11 (50MHz) => 0xF
  GPIOB->CRH &= 0xFFFF00FF; // 清空 PB10, PB11
  GPIOB->CRH |= 0x0000FF00; // 配置为 0xF (AF_OD 50MHz)

  // 3. 配置 I2C2 参数
  I2C2->CR1 |= I2C_CR1_SWRST;  // 软件复位 I2C2 (防卡死的好习惯)
  I2C2->CR1 &= ~I2C_CR1_SWRST; // 结束复位

  // 设置 I2C 频率 (这里假设 APB1 总线频率为 8MHz，如果你开了 36M，这里会自动对)
  // 3.1 获取 PCLK1 频率 (单位 MHz)
  uint32_t pclk1 = 8; // 默认 8MHz。如果 SystemCoreClock 是 72M，这里应该是 36
  // 如果你想更严谨，可以用 SystemCoreClock / (APB1预分频) 动态算

  I2C2->CR2 = pclk1; // 告诉 I2C 模块输入时钟是多少 MHz

  // 3.2 设置 CCR (时钟控制寄存器)
  // 标准模式 100kHz。公式：CCR = PCLK1 / (2 * 100000)
  // 8MHz 下：8000000 / 200000 = 40 (0x28)
  I2C2->CCR = pclk1 * 1000000 / 200000;

  // 3.3 设置 TRISE (最大上升时间)
  // 公式：(PCLK1 MHz) + 1
  I2C2->TRISE = pclk1 + 1;

  // 4. 使能 I2C2 模块
  I2C2->CR1 |= I2C_CR1_PE;
}
// 向设备写一个字节数据
void I2C2_WriteByte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data) {
  // 1. 发送起始信号
  I2C2->CR1 |= I2C_CR1_START;
  while (!(I2C2->SR1 & I2C_SR1_SB))
    ; // 等待 SB (Start Bit) 置位

  // 2. 发送设备地址 (写模式)
  I2C2->DR = dev_addr;
  while (!(I2C2->SR1 & I2C_SR1_ADDR))
    ;              // 等待地址发送完成
  (void)I2C2->SR2; // 读取 SR2 清除 ADDR 标志位 (这是硬件特性，必须读一下 SR2)

  // 3. 发送寄存器地址
  I2C2->DR = reg_addr;
  while (!(I2C2->SR1 & I2C_SR1_TXE))
    ; // 等待数据寄存器空

  // 4. 发送数据
  I2C2->DR = data;
  while (!(I2C2->SR1 & I2C_SR1_BTF))
    ; // 等待字节传输完成 (Byte Transfer Finished)

  // 5. 发送停止信号
  I2C2->CR1 |= I2C_CR1_STOP;
}
// 从设备读取一个字节数据
uint8_t I2C2_ReadByte(uint8_t dev_addr, uint8_t reg_addr) {
  uint8_t data;

  // --- 第一阶段：写寄存器地址 ---
  I2C2->CR1 |= I2C_CR1_START;
  while (!(I2C2->SR1 & I2C_SR1_SB))
    ;

  I2C2->DR = dev_addr; // 写地址
  while (!(I2C2->SR1 & I2C_SR1_ADDR))
    ;
  (void)I2C2->SR2;

  I2C2->DR = reg_addr; // 目标寄存器
  while (!(I2C2->SR1 & I2C_SR1_TXE))
    ; // 等发完

  // --- 第二阶段：重启 I2C 读数据 ---
  I2C2->CR1 |= I2C_CR1_START; // 重复起始信号 (Restart)
  while (!(I2C2->SR1 & I2C_SR1_SB))
    ;

  I2C2->DR = dev_addr | 1; // 读地址 (最低位为 1)
  while (!(I2C2->SR1 & I2C_SR1_ADDR))
    ;

  // 关键步骤：只读 1 个字节时，要在清除 ADDR 之前就把 ACK 关掉
  I2C2->CR1 &= ~I2C_CR1_ACK; // 不应答 (NACK)，告诉从机别发了
  (void)I2C2->SR2;           // 清除 ADDR
  I2C2->CR1 |= I2C_CR1_STOP; // 预先准备停止信号

  while (!(I2C2->SR1 & I2C_SR1_RXNE))
    ; // 等待数据收到
  data = I2C2->DR;

  I2C2->CR1 |= I2C_CR1_ACK; // 恢复 ACK，方便下次使用

  return data;
}

// 极简 I2C 发送字节
void I2C_Send(uint8_t byte) {
  for (int i = 0; i < 8; i++) {
    if (byte & 0x80)
      SDA_H;
    else
      SDA_L;
    // 这里需要一点点延迟，如果你的 SystemCoreClock 是 8MHz，空循环几次即可
    for (volatile int d = 0; d < 1000; d++)
      ;
    SCL_H;
    for (volatile int d = 0; d < 1000; d++)
      ;
    SCL_L;
    byte <<= 1;
  }
  // 释放 SDA 等待 ACK（这里简单处理，直接给个脉冲）
  SDA_H;
  SCL_H;
  for (volatile int d = 0; d < 10; d++)
    ;
  SCL_L;
}

// 停止信号：SCL高电平时，SDA由低变高
void I2C_Stop(void) {
  SDA_L; // 先确保 SDA 是低的
  for (volatile int d = 0; d < 100; d++)
    ;
  SCL_H; // 拉高 SCL
  for (volatile int d = 0; d < 100; d++)
    ;
  SDA_H; // 在 SCL 高电平期间拉高 SDA，触发 Stop
  for (volatile int d = 0; d < 100; d++)
    ;
}

// OLED 写命令函数
void OLED_Cmd(uint8_t cmd) {
  // --- Start 信号 ---
  SDA_H;
  SCL_H;
  for (volatile int d = 0; d < 10; d++)
    ; // 极短延时即可
  SDA_L;
  for (volatile int d = 0; d < 10; d++)
    ;
  SCL_L;
  I2C_Send(0x78); // OLED 地址
  I2C_Send(0x00); // 命令模式
  I2C_Send(cmd);  // 发送指令
  I2C_Stop();
}

void OLED_GPIO_Init(void) {

  // 1. 开启 GPIOB 时钟
  RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

  // 2. 配置 PB8 和 PB9
  // CRH 寄存器中，每 4 位控制一个引脚。
  // PB8 对应 CRH 的 0-3 位，PB9 对应 4-7 位。
  // 0x7 代表：通用开漏输出 (01) + 最大速度 50MHz (11) -> 0111 (二进制)
  GPIOB->CRH &= ~(0xFF << 0); // 清除 PB8, PB9 的配置位
  GPIOB->CRH |= (0x77 << 0);  // 设置 PB8, PB9 为开漏输出 50MHz
  // 3. 初始状态设为高电平（I2C 总线空闲状态）
  GPIOB->BSRR = (GPIO_BSRR_BS8 | GPIO_BSRR_BS9);
  // OLED_Cmd(0xAE);
  // OLED_Cmd(0xD5);
  // OLED_Cmd(0x80);

  // OLED_Cmd(0xA8);
  // OLED_Cmd(0x3F);

  // OLED_Cmd(0xD3);
  // OLED_Cmd(0x00);
  // OLED_Cmd(0x40);
  // OLED_Cmd(0xA1);
  // OLED_Cmd(0xC8);
  // OLED_Cmd(0xDA);
  // OLED_Cmd(0x12);
  // OLED_Cmd(0x81);
}
void OLED_Clear(volatile uint8_t *data) {
  printf("oled clearing , it's time coster");
  for (uint8_t i = 0; i < 8; i++) {
    OLED_Cmd(0xB0 + i); // 设置页地址 (0-7)
    OLED_Cmd(0x00);     // 设置起始列低地址
    OLED_Cmd(0x10);     // 设置起始列高地址

    // 开始发数据
    SDA_H;
    SCL_H;
    SDA_L;
    SCL_L; // Start
    I2C_Send(0x78);
    I2C_Send(0x40); // 0x40 代表接下来全是数据（Data）
    for (uint8_t j = 0; j < 128; j++) {
      I2C_Send(*data); // 0x00 全黑, 0xFF 全亮
    }
    SCL_H;
    SDA_H; // Stop
  }
}
void I2C_Delay(void) {
  for (volatile int i = 0; i < 50; i++)
    ; // 8MHz 下约 10-20us，保证信号爬升
}

// 软件模拟 I2C 起始信号
void I2C_Start(void) {
  SDA_H;
  SCL_H;
  I2C_Delay();
  SDA_L;
  I2C_Delay(); // SCL 高电平时 SDA 下降沿
  SCL_L;
  I2C_Delay();
}

// 发送一个字节并检查 ACK
int I2C_SendByte_CheckACK(uint8_t byte) {
  // 1. 发送 8 位数据
  for (int i = 0; i < 8; i++) {
    if (byte & 0x80)
      SDA_H;
    else
      SDA_L;
    I2C_Delay();
    SCL_H;
    I2C_Delay();
    SCL_L;
    byte <<= 1;
  }
  // 2. 读取 ACK
  SDA_H; // 释放 SDA 让从机控制
  I2C_Delay();
  SCL_H;
  I2C_Delay();
  int ack = !SDA_READ; // 从机拉低 SDA 代表有应答 (ACK)
  SCL_L;
  return ack;
}
// PA0按键事件
void EXTI0_IRQHandler(void) {
  brightness = TIM3->CNT;
  // 检查是不是 EXTI0 触发的中断
  if (EXTI->PR & (1 << 0)) {
    // 执行你的逻辑：比如翻转 LED
    brightness += 5;
    if (brightness > 100) {
      TIM3->CNT = 100; // 如果超了，强行拉回 100
    } else {
      TIM3->CNT += 5;
    }
  }
  GPIOC->ODR ^= (1 << 13);
  flag = 1;
  fill_state ^= 0xFF;
  printf("flag:%d\n", flag);
  printf("fill_state:%x\n", fill_state);

  // 【极其重要】清除中断标志位！
  // 如果不清除，CPU 会以为中断一直存在，卡死在里面
  EXTI->PR = (1 << 0);
}
// PA1按键事件
void EXTI1_IRQHandler(void) {
  ;
  // brightness = TIM3->CNT;
  // if (EXTI->PR & (1 << 1)) {
  //   EXTI->PR |= (1 << 1);
  //   brightness -= 5;
  //   if (brightness <= 0) {
  //     TIM3->CNT = 0;
  //   } else {
  //     TIM3->CNT -= 5; // 减少亮度
  //   }
  // }
}

void MPU6050_Init(void) {
  // 解除睡眠模式：向 PWR_MGMT_1 (0x6B) 写入 0x00
  I2C2_WriteByte(MPU6050_ADDR, 0x6B, 0x00);
}
void UART1_SendChar(char c) {
  while (!(USART1->SR & USART_SR_TXE))
    ; // 等待发送缓冲区空
  USART1->DR = c;
}

// 职业动作：重定向（针对 GCC/Clang 工具链）
int _write(int file, char *ptr, int len) {
  for (int i = 0; i < len; i++) {
    UART1_SendChar(ptr[i]);
  }
  return len;
}
// PA9(TX);PA10(RX)
void USART_init(void) {
  // 开启USART1时钟
  RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN; // 开启 GPIOA 时钟
  // PA9 (TX): 0xB (1011) -> 复用推挽, 50MHz
  GPIOA->CRH &= ~(0xF << 4);
  GPIOA->CRH |= (0xB << 4);
  // PA10 (RX): 0x8 (1000) -> 输入带上拉/下拉
  GPIOA->CRH &= ~(0xF << 8);
  GPIOA->CRH |= (0x4 << 8); // 浮空输入即可

  USART1->BRR = 0x271;         // 72MHz 下 115200 波特率,8MHz就是12800
  USART1->CR1 |= USART_CR1_UE; // 使能 USART
  USART1->CR1 |= USART_CR1_TE; // 使能发送 (Transmitter)
  USART1->CR1 |= USART_CR1_RE; // 使能接收 (Receiver)
}

void button_init() {

  RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN);
  // 将 PA0 设为上拉输入 (使能内部电阻，让电平稳定在高位)
  GPIOA->CRL &= ~(0xF << 0);
  GPIOA->CRL |= (0x8 << 0); // 1000: Input with pull-up / pull-down
  GPIOA->ODR |= (1 << 0);   // PA0 设为上拉，防止引脚电平“悬空”乱跳
  // PA1
  GPIOA->CRL &= ~(0xF << 4);
  GPIOA->CRL |= (0x8 << 4); // 1000: Input with pull-up / pull-down
  GPIOA->ODR |= (1 << 1);
  // 2. 将 PA0 映射到 EXTI0 线
  // AFIO->EXTICR[0] 负责控制引脚 0-3
  AFIO->EXTICR[0] &= ~(0xF << 0); // 清零，默认 0000 就是 PA0
  // PA1映射
  // AFIO->EXTICR[0] &= ~(0xF << 4); // 清零，默认 0000 就是 PA0
  // 3. 配置 EXTI 控制器
  EXTI->IMR |= (1 << 0); // 允许线 0 的中断请求
  EXTI->IMR |= (1 << 1);
  EXTI->FTSR |= (1 << 0); // 设置下降沿触发 (按键按下瞬间)
  EXTI->FTSR |= (1 << 1); // 设置下降沿触发 (按键按下瞬间)
  EXTI->PR |= (1 << 0);   // 2. 关键：先清空标志位，把历史积欠“抹平”
  EXTI->PR |= (1 << 1);   // 2. 关键：先清空标志位，把历史积欠“抹平”
  //  4. 配置 NVIC (给中断“开绿灯”)
  //  EXTI0 的中断通道号通常是 6
  //
  NVIC_EnableIRQ(EXTI0_IRQn);
  // NVIC_EnableIRQ(EXTI1_IRQn);

  // PA2
  GPIOA->CRL &= ~(0xF << 8);      // clear
  GPIOA->CRL |= (0x8 << 8);       //
  GPIOA->ODR |= (1 << 2);         // 1是上拉
  AFIO->EXTICR[0] &= ~(0xF << 8); // 清除 EXTI2 的映射
  // 在 STM32 的设计中，这 4 位的值决定了选择哪个端口：
  // 0000 (0x0)：代表 GPIOA
  // 0001 (0x1)：代表 GPIOB
  // 0010 (0x2)：代表 GPIOC
  AFIO->EXTICR[0] |= (0x0 << 8);   // 将 EXTI2 映射到 GPIOA (PA2)
  EXTI->IMR |= (1 << 2);           // 开放 EXTI2 的中断屏蔽（允许请求）
  EXTI->FTSR |= (1 << 2);          // 设置 EXTI2 为下降沿触发 (Falling Trigger)
  EXTI->RTSR &= ~(1 << 2);         // 禁用上升沿触发（可选，确保只有按下触发）
  NVIC_EnableIRQ(EXTI2_IRQn);      // 开启 EXTI2 中断通道
  NVIC_SetPriority(EXTI2_IRQn, 2); // 设置优先级为 2（根据需要调整）
}
void EXTI2_IRQHandler(void) {
  // 检查是否真的是 EXTI2 触发的中断标志位
  if (EXTI->PR & (1 << 2)) {
    // --- 这里写你的逻辑 ---
    // 比如：一键关灯
    TIM3->CNT = 0;
    brightness = 0;
    // --------------------

    // 【非常重要】手动清除中断标志位
    // 在 STM32 中，往该位写 1 才是清除标志
    EXTI->PR = (1 << 2);
  }
}

void update_brightness(uint8_t is_add) {
  if (is_add) {
    if (TIM3->CNT <= 95)
      TIM3->CNT += 5;
    else
      TIM3->CNT = 100;
  } else {
    if (TIM3->CNT >= 5)
      TIM3->CNT -= 5;
    else
      TIM3->CNT = 0;
  }
}
// 这里用到了静态变量数组，基础虽然硬，但逻辑很“产品”
void handle_key_logic(uint8_t pin_num, volatile KeyState_t *state,
                      uint8_t is_add) {
  uint8_t key_down = !(GPIOA->IDR & (1 << pin_num));
  static uint16_t lp_timers[2] = {0, 0}; // 记录两个按键的长按时间
  static uint16_t ar_timers[2] = {0, 0}; // 记录两个按键的连发时间

  switch (*state) {
  case KEY_STATE_IDLE:
    if (key_down)
      *state = KEY_STATE_DEBOUNCE;
    break;

  case KEY_STATE_DEBOUNCE:
    if (key_down) {
      // 执行动作
      update_brightness(is_add);
      lp_timers[pin_num] = 0;
      *state = KEY_STATE_PRESSED;
    } else
      *state = KEY_STATE_IDLE;
    break;

  case KEY_STATE_PRESSED:
    if (key_down) {
      if (++lp_timers[pin_num] >= 50) { // 500ms
        ar_timers[pin_num] = 0;
        *state = KEY_STATE_WAIT_RELEASE;
      }
    } else
      *state = KEY_STATE_IDLE;
    break;

  case KEY_STATE_WAIT_RELEASE:
    if (key_down) {
      if (++ar_timers[pin_num] >= 10) { // 100ms 连发
        ar_timers[pin_num] = 0;
        update_brightness(is_add);
      }
    } else
      *state = KEY_STATE_IDLE;
    break;
  }
}

// 统一的亮度更新函数，防止溢出
// TIM中断定时
void Timer2_Init(void) {

  // 1. 开启 TIM2 时钟
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

  // 2. 设置预分频和自动重装值
  // 72000,000 / 72 = 1000,000Hz (1us 计数一次)
  TIM2->PSC = 71;
  // 1000,000 / 100 = 10,000Hz (0.1ms 中断一次)
  TIM2->ARR = 99;

  // 3. 开启中断更新控制位
  TIM2->DIER |= TIM_DIER_UIE;

  // 4. 在 NVIC 中使能 TIM2 中断
  NVIC_EnableIRQ(TIM2_IRQn);

  // 5. 启动定时器
  TIM2->CR1 |= TIM_CR1_CEN;
}
// PWM调光中断
void TIM2_IRQHandler(void) {
  if (TIM2->SR & TIM_SR_UIF) { // 检查更新标志
    TIM2->SR &= ~TIM_SR_UIF;   // 清除标志位
    // 1. 获取当前硬件计数值
    uint16_t current_val = TIM3->CNT;

    // 2. 边界判定与死锁 (假设 ARR 设为 65535 比较好判断)
    // 如果向右转超过了 100
    if (current_val > 100 && current_val < 32768) {
      TIM3->CNT = 100; // 强制拉回 100
      current_val = 100;
    }
    // 如果向左转越过了 0 (会跳到 65535 附近)
    else if (current_val > 32768) {
      TIM3->CNT = 0; // 强制拉回 0
      current_val = 0;
    }

    // 3. 更新全局亮度变量
    brightness = current_val;

    pwm_counter++;
    if (pwm_counter >= 100) {
      pwm_counter = 0;
    }

    // 逻辑：如果计数器小于亮度等级，就关灯（PC13 通常低电平亮，所以 BS 是灭）
    // 这里的逻辑基于你的 PC13 是低电平点亮还是高电平点亮
    if (pwm_counter < brightness) {
      // 高位写1,高电平
      GPIOC->BSRR = (1 << (13 + 16));
    } else {
      // 低位写1,低电平
      GPIOC->BSRR = (1 << 13);
    }
    // --- 2. 状态机按键扫描 (每10ms扫描一次比较稳) ---
    static uint8_t scan_timer = 0;
    if (++scan_timer >= 100) { // 假设 TIM2 是 1ms 一次，这里就是 10ms
      scan_timer = 0;

      // 读取 PA1 电平（假设按键在 PA1）
      uint8_t key_down = !(GPIOA->IDR & (1 << 1));

      switch (g_button_state) {
      case KEY_STATE_IDLE:
        if (key_down)
          g_button_state = KEY_STATE_DEBOUNCE;
        break;

      case KEY_STATE_DEBOUNCE:
        if (key_down) {
          // 【单击触发】：第一次按下立刻执行一次减法
          if (TIM3->CNT >= 5)
            TIM3->CNT -= 5;
          else
            TIM3->CNT = 0;

          long_press_timer = 0; // 重置长按计时
          g_button_state = KEY_STATE_PRESSED;
        } else {
          g_button_state = KEY_STATE_IDLE;
        }
        break;

      case KEY_STATE_PRESSED:
        // 如果还按着，就进入等待松手状态
        if (key_down) {
          long_press_timer++;
          // 检查是否达到长按时间阈值
          if (long_press_timer >= LONG_PRESS_TIME) {
            auto_repeat_timer = 0;                   // 准备开始连发
            g_button_state = KEY_STATE_WAIT_RELEASE; // 复用此状态或新开长按状态
          }
        } else {
          g_button_state = KEY_STATE_IDLE;
        }
        break;
      case KEY_STATE_WAIT_RELEASE:
        if (key_down) {
          // --- 【核心：长按连减逻辑】 ---
          auto_repeat_timer++;
          if (auto_repeat_timer >= REPEAT_SPEED) {
            auto_repeat_timer = 0; // 再次计时

            // 执行减法
            if (TIM3->CNT >= 5)
              TIM3->CNT -= 5;
            else
              TIM3->CNT = 0;
          }
        } else {
          g_button_state = KEY_STATE_IDLE;
        }
        break;
      }
      handle_key_logic(0, &g_buttona0_state, 1); // 1 代表加
    }
  }
}
// 编码器
void Encoder_Init(void) {
  // === 1. 开启时钟 (对应图片 APB1/APB2 ClockCmd) ===
  RCC->APB1ENR |= RCC_APB1ENR_TIM3EN; // 开启 TIM3 时钟
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN; // 开启 GPIOA 时钟

  // === 2. 配置 GPIOA 6 & 7 (对应图片 GPIO_Mode_IPU) ===
  // 清除 PA6, PA7 的配置位 (24-31位)
  GPIOA->CRL &= ~(0xFF << 24);
  // 设置为 1000: 上拉/下拉输入模式 (0x8)
  GPIOA->CRL |= (0x88 << 24);
  // ODR 对应位写 1，确保是“上拉” (江协视频中 IPU 的关键)
  GPIOA->ODR |= (GPIO_ODR_ODR6 | GPIO_ODR_ODR7);

  // === 3. 时基单元配置 (对应图片 TIM_TimeBaseInit) ===
  // 虽然编码器模式会自动接管部分计数逻辑，但 ARR 决定了调光范围
  TIM3->PSC = 0; // 分频为 1 (1-1)
  // TIM3->ARR = 100 - 1;       // 周期设为 100，对应 0-99 亮度
  TIM3->ARR = 0xFFFF - 1;    // 周期设为 100，对应 0-99 亮度
  TIM3->CR1 &= ~TIM_CR1_CKD; // 对应 TIM_CKD_DIV1

  // === 4. 输入捕获与滤波器配置 (对应图片 TIM_ICInit) ===
  // CC1S=01, CC2S=01: 通道映射 TI1->CH1, TI2->CH2
  // IC1F=0xF, IC2F=0xF: 最强滤波器，滤除机械编码器毛刺 (0x0F)
  TIM3->CCMR1 = (0x0F << 4) | (0x01 << 0) | (0x0F << 12) | (0x01 << 8);

  // === 5. 编码器接口配置 (对应图片 TIM_EncoderInterfaceConfig) ===
  // SMS=011: 编码器模式 3 (在 TI1 和 TI2 的边缘都计数，精度最高)
  TIM3->SMCR &= ~TIM_SMCR_SMS;
  TIM3->SMCR |= (0x03 << 0);

  // CC1P=0, CC2P=0: 极性不反转 (对应 Rising)
  TIM3->CCER &= ~(TIM_CCER_CC1P | TIM_CCER_CC2P);
  TIM3->CNT = 50;

  TIM3->CR1 |= TIM_CR1_CEN; // 开启计数器
}
int main(void) {
  SetClock72M();
  SystemCoreClockUpdate();
  SysTick_Config(SystemCoreClock / 1000);
  // I2C1使能
  RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
  RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
  RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
  USART_init();
  printf("SystemCoreClock: %d \n", SystemCoreClock);
  //
  GPIOB->CRH &= ~(0xF << 24);
  GPIOB->CRH |= (0x3 << 24);
  GPIOB->BSRR = GPIO_BSRR_BR14;
  button_init();
  Encoder_Init();
  // Encode_cgpt();
  Timer2_Init();

  //  OLED_GPIO_Init();
  //  OLED_Cmd(0x8D); // 设置电荷泵
  //  OLED_Cmd(0x14); // 开启
  //  OLED_Cmd(0xAF); // 开启显示
  //   I2C ack
  // 3. 配置 PC13 为推挽输出 (LED)

  GPIOC->CRH &= ~(0xF << 20);
  GPIOC->CRH |= (0x3 << 20);
  GPIOC->BSRR = GPIO_BSRR_BR13; // c13低电平亮

  // 4. 执行握手测试 (OLED 地址通常是 0x78)
  // I2C_Start();
  // if (I2C_SendByte_CheckACK(0x78)) {
  //  // 握手成功！点亮 PC13 (通常低电平亮)
  //  GPIOB->BSRR = GPIO_BSRR_BS13;
  //}

  /* 2. 配置 PC13 引脚模式 */
  /* CRH 是高 8 位引脚寄存器，每个引脚占 4 位。PC13 对应 [23:20] 位 */
  /* 先清除原有的配置，再设置为 0x3 (通用推挽输出，50MHz) */
  // GPIOC->CRH &= ~(0xF << 20);
  // GPIOC->CRH |= (0x3 << 20);

  /* 3. 设置初始状态（关键点！） */
  // 让 PC13 初始为低电平，PA6 初始为高电平
  // GPIOC->ODR &= ~(1 << 13);
  // ~ 是取反操作
  // &是与运算, &=是运算并赋值
  // ODR(Output Data Register)

  printf("System Startup...\n");

  while (1) {

    // 1. 获取当前计数值
    uint16_t current_cnt = TIM3->CNT;

    //// 2. 修正 printf 格式化字符串
    //// %u 用于无符号整数，不要用 %ld (它是给 32 位长整型的)
    printf("Brightness: %u\n", current_cnt);

    //// 3. 【关键】加一个延时，防止串口疯狂输出导致 CPU 没空管 PWM 调光
    for (volatile int i = 0; i < 500000; i++)
      ;
  }
}
// I2C2_Init();
// MPU6050_Init();
// uint8_t chip_id;
// int16_t accel_x;
// uint8_t data_h, data_l;

//    if (flag) {
//      printf("fill_state,%d", fill_state);
//      printf("get flag");
//      OLED_Clear(fill_state);
//      flag = 0;
//    }
//
// 2. 读取 WHO_AM_I (寄存器 0x75)，验证 I2C 是否通了
// 正常应该返回 0x68
// chip_id = I2C2_ReadByte(MPU6050_ADDR, 0x75);
// printf("MPU ID: 0x%X\n", chip_id);

//// 3. 读取加速度 X轴 (高8位 0x3B, 低8位 0x3C)
// data_h = I2C2_ReadByte(MPU6050_ADDR, 0x3B);
// data_l = I2C2_ReadByte(MPU6050_ADDR, 0x3C);
// accel_x = (data_h << 8) | data_l; // 合成 16 位数据

// printf("Accel X: %d\n", accel_x);
//  下面的功能放在了SysTickHandler,以更加高效
//  if (ms_ticks >= next_pc13) {
//    GPIOC->ODR ^= GPIO_ODR_ODR13;

//  // OLED_Clear(0xFF); // 所有的像素点都会被填满，屏幕变全白
//  //
//  next_pc13 = ms_ticks + 2000;
//}

//// 检查是否到了 PA6 该闪烁的时间
// if (ms_ticks >= next_pa6) {
//   GPIOA->ODR ^= GPIO_ODR_ODR6;
//   next_pa6 = ms_ticks + 300;
// }

// 这里的代码不会被阻塞，CPU 还可以同时处理其他事情
