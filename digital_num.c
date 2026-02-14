#include "stm32f103xb.h"
#include "system_stm32f1xx.h"

// 共阳极数码管字模表 (0 代表亮，1 代表灭)
// 数据位对应: DP G F E D C B A
const uint8_t SEG_FONT[] = {
    0xC0, // 0: 1100 0000 (G和DP灭，其他亮)
    0xF9, // 1: 1111 1001
    0xA4, // 2: 1010 0100
    0xB0, // 3: 1011 0000
    0x99, // 4: 1001 1001
    0x92, // 5: 1001 0010
    0x82, // 6: 1000 0010
    0xF8, // 7: 1111 1000
    0x80, // 8: 1000 0000
    0x90  // 9: 1001 0000
};

// 显存数组：保存 4 个位置当前应该显示的数字
// 需求：初始化先显示最大四位数字（9999）
volatile uint8_t display_buf[4] = {9, 9, 9, 9};
void Seg_Hardware_Init(void)
{
    // 1. 开启时钟：GPIOA, GPIOB, 以及 AFIO (复用功能时钟)
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    // 2. 释放 PB3 和 PB4 (关闭 JTAG，保留 SWD，否则接上去没反应)
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    // 3. 配置引脚为推挽输出 50MHz (0x3)
    // 3. 配置引脚为推挽输出 50MHz (模式码: 0x3)

    // [配置 GPIOA] PA11, PA12 (对应 CRH 的第 12~19 位)
    GPIOA->CRH &= ~(0xFF << 12); // 清零
    GPIOA->CRH |= (0x33 << 12);  // 设为推挽输出

    // [配置 GPIOB] PB3, PB4, PB5, PB6, PB7 (对应 CRL 的第 12~31 位)
    GPIOB->CRL &= ~(0xFFFFF0FF);
    GPIOB->CRL |= (0x33333033);

    // [配置 GPIOB] PB8 (对应 CRH 的第 0~3 位) 以及 PB12~PB15 (对应 CRH 的第 16~31 位)
    GPIOB->CRH &= ~(0xFFFF000F); // 精准掩码清零
    GPIOB->CRH |= (0x33330003);  // 精准设为推挽输出
}
// BSRR版本
void Output_Segment_Data(uint8_t font_data)
{
    // 1. 全部置高 (熄灭共阳极数码管)
    // 必须包含所有参与驱动的引脚：PB0, PB1, PB3, PB4, PB5, PB6, PB7, PB8
    uint32_t reset_mask = GPIO_BSRR_BS0 | GPIO_BSRR_BS1 | GPIO_BSRR_BS3 |
                          GPIO_BSRR_BS4 | GPIO_BSRR_BS5 | GPIO_BSRR_BS6 |
                          GPIO_BSRR_BS7 | GPIO_BSRR_BS8;
    GPIOB->BSRR = reset_mask;

    // 2. 提取需要点亮的位 (0代表亮)，拉低对应的引脚
    uint32_t set_low = 0;
    if (!(font_data & 0x01))
        set_low |= GPIO_BSRR_BR3; // A -> PB3
    if (!(font_data & 0x02))
        set_low |= GPIO_BSRR_BR4; // B -> PB4
    if (!(font_data & 0x04))
        set_low |= GPIO_BSRR_BR5; // C -> PB5
    if (!(font_data & 0x08))
        set_low |= GPIO_BSRR_BR6; // D -> PB6
    if (!(font_data & 0x10))
        set_low |= GPIO_BSRR_BR7; // E -> PB7
    if (!(font_data & 0x20))
        set_low |= GPIO_BSRR_BR8; // F -> PB8
    if (!(font_data & 0x40))
        set_low |= GPIO_BSRR_BR1; // G -> PB0
    if (!(font_data & 0x80))
        set_low |= GPIO_BSRR_BR0; // DP -> PB1

    GPIOB->BSRR = set_low;
}
// ODR版本
// void Output_Segment_Data(uint8_t font_data)
// {
//     // 1. 准备两个临时的 32 位变量来存储最终电平状态
//     uint32_t odr_a = GPIOA->ODR;
//     uint32_t odr_b = GPIOB->ODR;

//     // 2. 根据 font_data 计算每一位的状态 (0 亮 1 灭)
//     // A-F 段在 GPIOB
//     if (font_data & 0x01)
//         odr_b |= (1 << 3);
//     else
//         odr_b &= ~(1 << 3); // A
//     if (font_data & 0x02)
//         odr_b |= (1 << 4);
//     else
//         odr_b &= ~(1 << 4); // B
//     if (font_data & 0x04)
//         odr_b |= (1 << 5);
//     else
//         odr_b &= ~(1 << 5); // C
//     if (font_data & 0x08)
//         odr_b |= (1 << 6);
//     else
//         odr_b &= ~(1 << 6); // D
//     if (font_data & 0x10)
//         odr_b |= (1 << 7);
//     else
//         odr_b &= ~(1 << 7); // E
//     if (font_data & 0x20)
//         odr_b |= (1 << 8);
//     else
//         odr_b &= ~(1 << 8); // F

//     // G 和 DP 在 GPIOA
//     if (font_data & 0x40)
//         odr_a |= (1 << 11);
//     else
//         odr_a &= ~(1 << 11); // G (PA11)
//     if (font_data & 0x80)
//         odr_a |= (1 << 12);
//     else
//         odr_a &= ~(1 << 12); // DP (PA12)

//     // 3. 一次性写入 ODR 寄存器，引脚电平只会跳变一次，彻底消除“先灭后亮”产生的竞争
//     GPIOA->ODR = odr_a;
//     GPIOB->ODR = odr_b;
// }
/**
 * @brief  暴露给用户的打印接口
 * @param  num: 要显示的数字 (0 ~ 9999)
 */
void printNum(uint16_t num)
{
    // 边界保护：如果超过 9999，强制限制为 9999
    if (num > 9999)
    {
        num = 9999;
    }

    // 数学拆分解码
    display_buf[0] = num / 1000;         // 千位
    display_buf[1] = (num % 1000) / 100; // 百位
    display_buf[2] = (num % 100) / 10;   // 十位
    display_buf[3] = num % 10;           // 个位
}

/**
 * @brief  数码管动态刷新函数
 * @note   **必须**放在定时器中断中 (如 SysTick_Handler 或 TIM2_IRQHandler)，每 1~2ms 调用一次
 */
// 全局变量，可在 main 中修改来改变亮度
volatile uint8_t num_brightness = 10; // 0 为全灭，10 为最亮

void Seg_Scan(void)
{
    static uint8_t current_digit = 0;
    static uint8_t pwm_tick = 0; // PWM 内部计数器 (0-9)

    // 1. 无论亮灭，先关闭所有位选 (消影)
    // 注意：共阳极数码管位选通常是高电平选通，所以 BRR 熄灭是正确的
    GPIOB->BRR = GPIO_BSRR_BS12 | GPIO_BSRR_BS13 | GPIO_BSRR_BS14 | GPIO_BSRR_BS15;

    // 2. 更新 PWM 计数器
    pwm_tick++;
    if (pwm_tick >= 10)
    {
        pwm_tick = 0;

        // 只有在 PWM 周期开始时，才切换到下一位数字
        // 这样可以保证每一位数字显示的亮度时间是公平的
        current_digit++;
        if (current_digit >= 4)
            current_digit = 0;
    }

    // 3. 判断当前是否在“点亮时间点”内
    // 如果当前计数值小于亮度等级，则执行点亮逻辑
    if (pwm_tick < num_brightness)
    {
        // A. 输出段选数据 (此时已避开损坏的 PB0，使用你重映射后的引脚)
        uint8_t num_to_show = display_buf[current_digit];
        Output_Segment_Data(SEG_FONT[num_to_show]);

        // B. 打开当前位选 (拉高)
        GPIOB->BSRR = (1 << (12 + current_digit));
    }
    // 否则，不打开位选，保持熄灭状态（即实现了 PWM 占空比控制）
}
/*
段选（加限流电阻）：
A段 (11脚) -> PB3
B段 (7脚)  -> PB4
C段 (4脚)  -> PB5
D段 (2脚)  -> PB6
E段 (1脚)  -> PB7
F段 (10脚) -> PB8
G段 (5脚)  -> PA11
DP段 (3脚) -> PA12
位选（直接连）：
COM1 (12脚) -> PB12
COM2 (9脚)  -> PB13
COM3 (8脚)  -> PB14
COM4 (6脚)  -> PB15
*/