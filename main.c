#include <stdint.h>
#include <stdio.h>
#include "stm32f103xb.h"
#include "system_stm32f1xx.h"
#include "tm1650.h"
// todo: 一个开关，开关整个机器
//  定义一个简单的超时常量
#define I2C_TIMEOUT 10000

#define MPU6050_ADDR 0xD0 // MPU6050 的 I2C 地址 (AD0 接地时)

void Servo_SetAngle(uint16_t angle);
void Servo_Rotate_Relative(uint16_t delta, uint8_t isadd);
//
volatile uint32_t seconds = 0;

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
volatile uint16_t current_angle = 90;    // [修复] 初始值设为90，与 main 中 Servo_SetAngle(90) 保持同步
// 必须加 volatile，防止编译器优化导致死循环
volatile uint32_t Delay_Timer = 0;
#define LONG_PRESS_TIME 50 // 50 * 10ms = 500ms 进入长按
#define REPEAT_SPEED 10    // 10 * 10ms = 100ms 触发一次减法
//
typedef enum
{
    KEY_STATE_IDLE,        // 空闲（松开）
    KEY_STATE_DEBOUNCE,    // 消抖（刚按下，观察中）
    KEY_STATE_PRESSED,     // 确认按下（已触发逻辑）
    KEY_STATE_WAIT_RELEASE // 等待松手
} KeyState_t;

volatile KeyState_t g_buttona1_state = KEY_STATE_IDLE;
volatile KeyState_t g_buttona0_state = KEY_STATE_IDLE;
void SetClock72M(void)
{
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
// 这个名字是固定的，不能改
void SysTick_Handler(void)
{
    static uint32_t count = 0;
    if (Delay_Timer != 0)
    {
        Delay_Timer--; // 每 1ms 减 1
    }
}
/**
 * @brief  毫秒级延时函数
 * @param  ms: 延时时长（毫秒）
 */
void delay_ms(uint32_t ms)
{
    Delay_Timer = ms; // 赋初始值
    while (Delay_Timer != 0)
        ; // 等待中断将其减为 0
}
volatile uint8_t led_b9_enable = 1;
volatile uint8_t num_brightness = 0;
// B10,B11,Hard I2C2
void I2C2_Init(void)
{
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
void I2C2_WriteByte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data)
{
    // 1. 发送起始信号
    I2C2->CR1 |= I2C_CR1_START;
    while (!(I2C2->SR1 & I2C_SR1_SB))
        ; // 等待 SB (Start Bit) 置位

    // 2. 发送设备地址 (写模式)
    I2C2->DR = dev_addr;
    while (!(I2C2->SR1 & I2C_SR1_ADDR))
        ;            // 等待地址发送完成
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
uint8_t I2C2_ReadByte(uint8_t dev_addr, uint8_t reg_addr)
{
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

void MPU6050_Init(void)
{
    // 解除睡眠模式：向 PWR_MGMT_1 (0x6B) 写入 0x00
    I2C2_WriteByte(MPU6050_ADDR, 0x6B, 0x00);
}
void UART1_SendChar(char c)
{
    while (!(USART1->SR & USART_SR_TXE))
        ; // 等待发送缓冲区空
    USART1->DR = c;
}

// 职业动作：重定向（针对 GCC/Clang 工具链）
int _write(int file, char *ptr, int len)
{
    for (int i = 0; i < len; i++)
    {
        UART1_SendChar(ptr[i]);
    }
    return len;
}

// PA9(TX);PA10(RX)
// Notice: 接线时和TTL,是TX->RX的关系
/*
在微控制器内部，串口控制器（USART1）的硬件电路是固定的，它的输出信号（TX/RX）只能连接到芯片预先设计好的特定引脚上。
对于 STM32F1 系列，USART1 的“老家”默认就在 PA9 (TX) 和 PA10 (RX)。
只能重映射引脚 (Remapped)：PB6 (TX), PB7 (RX)。
如果你想改用 PB6 和 PB7，你不能只改 GPIOA 为 GPIOB，你还必须多做两件事：
开启 AFIO（复用功能 IO）时钟。
在 AFIO->MAPR 寄存器中开启 USART1 的重映射位。
*/
void USART_init(void)
{
    // 开启USART1时钟
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN; // 开启 GPIOA 时钟
    // PA9 (TX): 0xB (1011) -> 复用推挽, 50MHz
    GPIOA->CRH &= ~(0xF << 4);
    GPIOA->CRH |= (0xB << 4);
    // PA10 (RX): 0x8 (1000) -> 输入带上拉/下拉
    GPIOA->CRH &= ~(0xF << 8);
    GPIOA->CRH |= (0x4 << 8); // 浮空输入即可
    // Baud Rate Register; 时钟频率/BBR = Baud Rate
    USART1->BRR = 0x271; // 72MHz下115200 波特率,8MHz就是12800

    // 使能USART;使能Transmitter;使能Receiver
    USART1->CR1 |= USART_CR1_UE | USART_CR1_RE | USART_CR1_TE;
}
// not used now
void EXIT_demo()
{
    // 2. 将 PA0 映射到 EXTI0 线
    AFIO->EXTICR[0];                // 负责控制引脚 0-3
    AFIO->EXTICR[0] &= ~(0xF << 0); // 清零，默认 0000 就是 PA0
                                    // PA1映射
    AFIO->EXTICR[0] &= ~(0xF << 8); // 清除 EXTI2 的映射
    AFIO->EXTICR[0] &= ~(0xF << 4); // 清零，默认 0000 就是 PA0
    // 3. 配置 EXTI 控制器
    EXTI->IMR |= (1 << 0); // 允许线 0 的中断请求
    EXTI->IMR |= (1 << 1);
    EXTI->FTSR |= (1 << 0); // 设置下降沿触发 (按键按下瞬间)
    EXTI->FTSR |= (1 << 1); // 设置下降沿触发 (按键按下瞬间)
    EXTI->PR |= (1 << 0);   // 2. 关键：先清空标志位，把历史积欠“抹平”
    EXTI->PR |= (1 << 1);   // 2. 关键：先清空标志位，把历史积欠“抹平”
                            // 4. 配置 NVIC (给中断“开绿灯”)
    // EXTI0 的中断通道号通常是 6
    //
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);

    // 在 STM32 的设计中，这 4 位的值决定了选择哪个端口：
    // 0000 (0x0)：代表 GPIOA
    // 0001 (0x1)：代表 GPIOB
    // 0010 (0x2)：代表 GPIOC
    AFIO->EXTICR[0] |= (0x0 << 8); // 将 EXTI2 映射到 GPIOA (PA2)
    EXTI->IMR |= (1 << 2);         // 开放 EXTI2 的中断屏蔽（允许请求）
    EXTI->FTSR |= (1 << 2);        // 设置 EXTI2 为下降沿触发 (Falling
                                   // Trigger) EXTI->RTSR &= ~(1 << 2);         //
                                   // 禁用上升沿触发（可选，确保只有按下触发） NVIC_EnableIRQ(EXTI2_IRQn); //
                                   // 开启 EXTI2 中断通道 NVIC_SetPriority(EXTI2_IRQn, 2); // 设置优先级为
                                   // 2（根据需要调整）
}

void button_init()
{
    /*
    1.
    在真正的 PCB 设计中，按键不会直接连到 MCU 引脚：
    外部上拉/下拉：不依赖芯片内部微弱的电阻，而是使用外部 10kΩ 电阻确保电平稳定。
    RC 滤波：在引脚和地之间并联一个 100nF 电容，通过物理方式过滤掉 80% 的高频机械抖动。
    ESD 保护：在按键处增加静电保护二极管，防止人体静电击穿单片机 IO 口。
    真正产品严禁使用 EXTI 中断做按键，也严禁使用 delay。
    */
    RCC->APB2ENR |= (RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN);
    // 将 PA0 设为上拉输入 (使能内部电阻，让电平稳定在高位)
    GPIOA->CRL &= ~(0xF << 0);
    GPIOA->CRL |= (0x8 << 0); // 1000: Input with pull-up / pull-down
    GPIOA->ODR |= (1 << 0);   // PA0 设为上拉，防止引脚电平“悬空”乱跳
    // PA1
    GPIOA->CRL &= ~(0xF << 4);
    GPIOA->CRL |= (0x8 << 4); // 1000: Input with pull-up / pull-down
    GPIOA->ODR |= (1 << 1);
    // PA2
    GPIOA->CRL &= ~(0xF << 8); // clear
    GPIOA->CRL |= (0x8 << 8);  //
    GPIOA->ODR |= (1 << 2);    // 1是上拉
    // PA4
    GPIOA->CRL &= ~(0xF << 16);
    GPIOA->CRL |= (0x8 << 16);
    GPIOA->ODR |= (1 << 4);
}

void EXTI2_IRQHandler(void)
{
    // 检查是否真的是 EXTI2 触发的中断标志位
    if (EXTI->PR & (1 << 2))
    {
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
/**
 * 相对转动函数
 * @param delta: 想要转动的增量（例如 90 或 -90）
 */
void Servo_Rotate_Relative(uint16_t delta, uint8_t isadd)
{
    if (isadd)
    {
        current_angle += delta;
        if (current_angle > 180)
            current_angle = 180;
    }
    else
    {
        // [修复] 防止无符号数减法溢出 (例如 0 - 90 会变成 65446)
        if (current_angle < delta)
            current_angle = 0;
        else
            current_angle -= delta;
    }

    // 3. 调用你之前的设置函数
    Servo_SetAngle(current_angle);
}
// 统一的亮度更新函数，防止溢出
void update_brightness(uint8_t is_add)
{
    if (is_add)
    {
        if (num_brightness < 10)
        {
            num_brightness++;
        }
        else
        {
            num_brightness = 10;
        }
        if (TIM3->CNT <= 95)
            TIM3->CNT += 5;
        else
            TIM3->CNT = 100;
    }
    else
    {
        if (num_brightness > 1)
        {
            num_brightness--;
        }
        else
        {
            num_brightness = 0;
        }
        num_brightness -= 1;
        if (TIM3->CNT >= 5)
            TIM3->CNT -= 5;
        else
            TIM3->CNT = 0;
    }
}
// 这里用到了静态变量数组，基础虽然硬，但逻辑很“产品”
void handle_key_logic(uint8_t pin_num, volatile KeyState_t *state,
                      uint8_t is_add)
{
    // todo:为了更通用，lp_timers和ar_timers要扩大容量
    uint8_t key_down = !(GPIOA->IDR & (1 << pin_num));
    static uint16_t lp_timers[2] = {0, 0}; // 记录两个按键的长按时间
    static uint16_t ar_timers[2] = {0, 0}; // 记录两个按键的连发时间

    switch (*state)
    {
    case KEY_STATE_IDLE:
        if (key_down)
            *state = KEY_STATE_DEBOUNCE;
        break;

    case KEY_STATE_DEBOUNCE:
        if (key_down)
        {
            // 执行动作
            update_brightness(is_add);
            Servo_Rotate_Relative(90, is_add);
            lp_timers[pin_num] = 0;
            *state = KEY_STATE_PRESSED;
        }
        else
            *state = KEY_STATE_IDLE;
        break;

    case KEY_STATE_PRESSED:
        if (key_down)
        {
            if (++lp_timers[pin_num] >= 50)
            { // 500ms
                ar_timers[pin_num] = 0;
                *state = KEY_STATE_WAIT_RELEASE;
            }
        }
        else
            *state = KEY_STATE_IDLE;
        break;

    case KEY_STATE_WAIT_RELEASE:
        if (key_down)
        {
            if (++ar_timers[pin_num] >= 10)
            { // 100ms 连发
                ar_timers[pin_num] = 0;
                update_brightness(is_add);
            }
        }
        else
            *state = KEY_STATE_IDLE;
        break;
    }
}

// TIM中断定时
void Timer2_Init(void)
{
    // 1. 开启 TIM2 时钟，挂载在APB1总线
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    // 2. 设置预分频和自动重装值
    // 72000,000 / 72 = 1000,000Hz (1us 计数一次)
    // 时钟慢放分母；Prescaler Register
    TIM2->PSC = 71;
    // 1000,000 / 100 = 10,000Hz (0.1ms 中断一次)
    // Auto-Reload Register;产生更新事件（中断）的计数条件
    TIM2->ARR = 99;

    // 3. 开启中断更新控制位
    // DMA/Interrupt Enable Register; UIE(Update Interrupt Enable)
    // 开启更新中断使能
    TIM2->DIER |= TIM_DIER_UIE;

    // 4. 在 NVIC 中使能 TIM2 中断
    NVIC_EnableIRQ(TIM2_IRQn);

    // 5. 计数器使能
    // CR(Control Register);CEN(Counter Enable)
    TIM2->CR1 |= TIM_CR1_CEN;
}
volatile uint8_t update_flag = 0;  // 触发标志位
volatile uint32_t timer_ticks = 0; // 中断计数器

volatile uint8_t cdown = 0;

// PWM调光
void TIM2_IRQHandler(void)
{
    static uint16_t led_flash_timer = 0;
    if (TIM2->SR & TIM_SR_UIF)
    {                            // 检查更新标志
        TIM2->SR &= ~TIM_SR_UIF; // 清除标志位

        timer_ticks++;
        // 1s = 10000 * 0.1ms
        // 这里正好是1s,可以给时钟使用
        if (timer_ticks >= 10000)
        {
            update_flag = 1; // 仅立一个Flag，不在这里执行耗时任务
            timer_ticks = 0; // 重置计数器
        }
        // 1. 获取当前硬件计数值
        uint16_t current_val = TIM3->CNT;

        // 2.边界判定与死锁(假设 ARR 设为 65535 比较好判断)
        // 如果向右转超过了 100
        if (current_val > 100 && current_val < 32768)
        {
            TIM3->CNT = 100; // 强制拉回 100
            current_val = 100;
        }
        // 如果向左转越过了 0 (会跳到 65535 附近)
        else if (current_val > 32768)
        {
            TIM3->CNT = 0; // 强制拉回 0
            current_val = 0;
        }
        num_brightness = current_val / 10;

        // 3. 更新全局亮度变量
        brightness = current_val;

        pwm_counter++;
        if (pwm_counter >= 100)
        {
            pwm_counter = 0;
        }
        // 逻辑：如果计数器小于亮度等级，就关灯（PC13 通常低电平亮，所以 BS 是灭）
        // 这里的逻辑基于你的 PC13 是低电平点亮还是高电平点亮
        if (pwm_counter < brightness)
        {
            // C13
            GPIOC->BSRR = (1 << (13 + 16));
            GPIOB->BSRR = GPIO_BSRR_BR0;
        }
        else
        {
            // C13
            GPIOC->BSRR = (1 << 13);
            GPIOB->BSRR = GPIO_BSRR_BS0;
        }
        // b9保持PWM调光的同时，固定频率闪烁
        // 这个需要单独的计数器控制频率，翻转状态
        led_flash_timer++;
        if (led_flash_timer >= 5000)
        {
            led_flash_timer = 0;
            led_b9_enable = !led_b9_enable;
        }
        if (led_b9_enable && pwm_counter < brightness)
        {
            GPIOB->BSRR = GPIO_BSRR_BR9;
        }
        else
        {
            GPIOB->BSRR = GPIO_BSRR_BS9;
        }

        // --- 2. 状态机按键扫描 (每10ms扫描一次比较稳) ---
        static uint8_t scan_timer = 0;
        if (++scan_timer >= 100)
        { //
            scan_timer = 0;
            uint8_t key_down = !(GPIOA->IDR & (1 << 4));
            if (key_down)
            {
                cdown = 1;
            }
            handle_key_logic(1, &g_buttona1_state, 0);
            handle_key_logic(0, &g_buttona0_state, 1); // 1 代表加
            TM1650_SetBrightness_Adaptive(brightness);
        }
    }
}
// 旋转编码器
void Encoder_Init(void)
{
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

void LED_init(void)
{ // 低电平亮
    // enable->config->bit set;标准的三步曲
    // B9
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    GPIOB->CRH &= ~(0xF << 4);
    GPIOB->CRH |= (0x3 << 4);
    GPIOB->BSRR = GPIO_BSRR_BR9;
    // B0
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    GPIOB->CRH &= ~(0xF << 0);
    GPIOB->CRH |= (0x3 << 0);
    GPIOB->BSRR = GPIO_BSRR_BR0;
    // C13
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH &= ~(0xF << 20);
    GPIOC->CRH |= (0x3 << 20);
    GPIOC->BSRR = GPIO_BSRR_BR13;
}

// 专门给舵机用的初始化，不影响你的 Timer2_Init
void Servo_Init(void)
{
    // 1. 开启 TIM1 (高级定时器) 和 GPIOA 的时钟
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    // 2. 配置 PA8 为复用推挽输出 (Alt Function Push-Pull)
    // PA8 对应 CRH 寄存器的第 0-3 位
    // 设置为 0xB (1011): 50MHz 复用推挽
    GPIOA->CRH &= ~(0xF << 0);
    GPIOA->CRH |= (0xB << 0);

    // 3. 设置定时器参数：产生 20ms 周期
    // 72MHz / 72 = 1MHz (1us 计数一次)
    TIM1->PSC = 71;
    // 计数 20000 次 = 20ms
    TIM1->ARR = 19999;

    // 4. 配置通道 1 (CH1) 为 PWM 模式 1
    TIM1->CCMR1 &= ~(0x7 << 4);     // 清除原来的模式
    TIM1->CCMR1 |= (6 << 4);        // 设置 OC1M = 110 (PWM Mode 1)
    TIM1->CCMR1 |= TIM_CCMR1_OC1PE; // 开启预装载

    // 5. 使能通道 1 输出开关
    TIM1->CCER |= TIM_CCER_CC1E;

    // 6. 【关键】高级定时器 TIM1 需要开启主输出使能 (MOE)
    TIM1->BDTR |= TIM_BDTR_MOE;

    // 7. 开启定时器
    TIM1->CR1 |= TIM_CR1_CEN;

    // 初始位置：90度 (1.5ms)
    TIM1->CCR1 = 1500;
}
// 对应的角度设置函数
void Servo_SetAngle(uint16_t angle)
{
    if (angle < 0)
        angle = 0;
    if (angle > 180)
        angle = 180;

    // 映射：0~180度 -> 500~2500us
    uint16_t compare = 500 + (angle * 2000 / 180);
    TIM1->CCR1 = compare;
}

int main(void)
{

    SetClock72M();
    SystemCoreClockUpdate();
    // 你每跳够 SystemCoreClock / 1000下，就给我报个信（进一次中断）”。这个不管CPU频率，都是1ms
    SysTick_Config(SystemCoreClock / 1000);

    USART_init();

    printf("SystemCoreClock: %d \n", SystemCoreClock);
    //
    GPIOB->CRH &= ~(0xF << 24);
    GPIOB->CRH |= (0x3 << 24);
    GPIOB->BSRR = GPIO_BSRR_BR14;
    button_init();
    Encoder_Init();
    Timer2_Init();
    LED_init();
    Servo_Init();
    Servo_SetAngle(90);
    tm1650_init();

    while (1)
    {
        if (update_flag)
        {
            seconds++;
            update_flag = 0;
            // 这里可以放一些需要定时执行的任务，例如更新时间显示
            Display_Time_Auto(seconds); // 传入总秒数，自动格式化显示
        }
    }
}
