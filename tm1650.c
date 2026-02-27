
#include "stm32f103xb.h"
#include "system_stm32f1xx.h"
// 0-9 的共阴极段码 (a-b-c-d-e-f-g)
uint8_t segment_map[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
/* I2C 底层原子操作 */
static inline void I2C_SCL(uint8_t bit)
{
    if (bit)
        GPIOB->BSRR = GPIO_BSRR_BS6;
    else
        GPIOB->BSRR = GPIO_BSRR_BR6;
}

static inline void I2C_SDA(uint8_t bit)
{
    if (bit)
        GPIOB->BSRR = GPIO_BSRR_BS7;
    else
        GPIOB->BSRR = GPIO_BSRR_BR7;
}

static inline uint8_t I2C_SDA_READ(void)
{
    return (GPIOB->IDR & GPIO_IDR_IDR7) ? 1 : 0;
}

/* 工业级精确延时：建议使用 DWT 计数器或简单的 NOP，不要用 for 循环 */
static void I2C_Delay(void)
{
    // 72MHz 下，约 20-30 个 NOP 可以达到 400KHz 速率
    for (volatile int i = 0; i < 50; i++)
        ;
}
void I2C_Stop(void)
{
    I2C_SDA(0); // 确保 SDA 为低
    I2C_Delay();
    I2C_SCL(1); // 释放时钟线
    I2C_Delay();
    I2C_SDA(1);  // SCL 高电平时，SDA 上升沿
    I2C_Delay(); // 确保停止信号维持足够时间
}

void I2C_Start(void)
{
    I2C_SDA(1);
    I2C_SCL(1);
    I2C_Delay();
    I2C_SDA(0); // SCL 高电平时，SDA 下降沿
    I2C_Delay();
    I2C_SCL(0); // 释放时钟线
    I2C_Delay();
}

/**
 * @brief  I2C 发送一个字节并等待应答
 * @return 0: 收到应答(ACK), 1: 无应答(NACK)
 */
uint8_t I2C_Send_Byte(uint8_t byte)
{
    // 1. 发送 8 位数据
    for (uint8_t i = 0; i < 8; i++)
    {
        I2C_SDA((byte & 0x80) >> 7);
        byte <<= 1;
        I2C_Delay();
        I2C_SCL(1);
        I2C_Delay();
        I2C_SCL(0);
    }

    // 2. 检查应答位 (第 9 个脉冲)
    I2C_SDA(1); // 释放 SDA，交给从机控制
    I2C_Delay();
    I2C_SCL(1); // 驱动时钟
    I2C_Delay();

    uint8_t ack = I2C_SDA_READ(); // 读取从机是否拉低了 SDA

    I2C_SCL(0); // 结束
    return ack; // 返回 0 表示成功
}

/**
 * @brief  向 TM1650 写入地址和数据
 * @return 0 成功, 1 失败
 */
uint8_t TM1650_Write(uint8_t addr, uint8_t data)
{
    I2C_Start();

    if (I2C_Send_Byte(addr) != 0)
    { // 检查地址是否有应答
        I2C_Stop();
        return 1;
    }

    if (I2C_Send_Byte(data) != 0)
    { // 检查数据是否有应答
        I2C_Stop();
        return 1;
    }

    I2C_Stop();
    return 0;
}
void tm1650_init(void)
{
    // 1. 开启 GPIOB 时钟
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    // PB6和PB7是 TM1650 的 SCL 和 SDA，必须配置为开漏输出
    GPIOB->CRL &= ~(0xFF << 24);

    // 设置为通用开漏输出 (Output Open-Drain), 50MHz
    // CNF = 01 (开漏), MODE = 11 (50MHz) -> 0x7
    GPIOB->CRL |= (0x77 << 24);

    // 3. 初始状态设为高电平（I2C 总线空闲状态）
    GPIOB->BSRR = (GPIO_BSRR_BS6 | GPIO_BSRR_BS7);
}
// 使用示例：在第一个位置显示数字 5

/**
 * @brief 设置亮度
 * @param brightness 1-7 (1最暗, 7最亮, 0是自动/8级)
 */
void TM1650_SetBrightness(uint8_t brightness)
{
    if (brightness > 7)
        brightness = 7;
    // 亮度在 Bit4-6，Bit0 为开关
    TM1650_Write(0x48, (brightness << 4) | 0x01);
}
/**
 * @brief 将 0-100 的亮度值映射到 TM1650 的硬件亮度等级
 * @param input_val 外部传入的 0-100 的变量
 */
void TM1650_SetBrightness_Adaptive(uint8_t input_val)
{
    uint8_t level;
    uint8_t control_data;

    if (input_val == 0)
    {
        // 如果输入为 0，直接关灯
        control_data = 0x00;
    }
    else
    {
        // 映射逻辑：将 1-100 映射到 1-7 级
        // 计算公式：level = (input_val * 7) / 100
        // 这样：100对应7级，50对应3级，1对应0级(其实也是一种亮度)
        level = (input_val * 7) / 101; // 除以101是为了防止边缘溢出，保证最大是7

        if (level > 7)
            level = 7;

        // 映射到 TM1650 的 Bit4-6
        // 注意：TM1650 的 000 代表第8级(通常最亮)，001代表第1级(最暗)
        // 为了逻辑简单，我们直接用 1-7 级映射
        if (level == 0)
            level = 1; // 只要不是0，就起码给点亮

        control_data = (level << 4) | 0x01; // Bit0=1 开启显示
    }

    TM1650_Write(0x48, control_data);
}

/**
 * @brief 自动格式化显示时间
 * @param total_seconds 总秒数
 */
void Display_Time_Auto(uint32_t total_seconds)
{
    uint8_t v1, v2, v3, v4;

    // 💡 优化1: 删除了这里的 volatile。对于单纯的局部计算变量，
    // 加 volatile 会阻止编译器进行寄存器优化，拖慢运行速度。
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds % 3600) / 60;
    uint32_t seconds = total_seconds % 60;

    if (hours == 0)
    {
        // 【MM:SS 格式】
        v1 = minutes / 10;
        v2 = minutes % 10;
        v3 = seconds / 10;
        v4 = seconds % 10;
    }
    else
    {
        // 【HH:MM 格式】
        uint32_t disp_hours = (hours > 99) ? 99 : hours;
        v1 = disp_hours / 10;
        v2 = disp_hours % 10;
        v3 = minutes / 10;
        v4 = minutes % 10;
    }

    // 写入数码管：
    // 💡 优化2: 修复高位消隐 bug。只判断真正的最高位 v1。
    if (v1 == 0)
    {
        TM1650_Write(0x68, 0x00); // 最高位为 0 时全灭（不显示前导零）
    }
    else
    {
        TM1650_Write(0x68, segment_map[v1]);
    }

    // 对于时间显示，v2, v3, v4 即使是 0 也必须显示出来，否则格式会乱
    // 第二位加上小数点 (0x80) 作为时钟中间的冒号
    TM1650_Write(0x6A, segment_map[v2] | 0x80);

    // 第三位、第四位正常显示
    TM1650_Write(0x6C, segment_map[v3]);
    TM1650_Write(0x6E, segment_map[v4]);
}
