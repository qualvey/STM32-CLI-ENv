
// 0-9 的共阴极段码 (a-b-c-d-e-f-g)
extern const uint8_t segment_map[];
/* I2C 底层原子操作 */
static inline void I2C_SCL(uint8_t bit);

static inline void I2C_SDA(uint8_t bit);

static inline uint8_t I2C_SDA_READ(void);

/* 工业级精确延时：建议使用 DWT 计数器或简单的 NOP，不要用 for 循环 */
static void I2C_Delay(void);

void I2C_Stop(void);

void I2C_Start(void);

/**
 * @brief  I2C 发送一个字节并等待应答
 * @return 0: 收到应答(ACK), 1: 无应答(NACK)
 */
uint8_t I2C_Send_Byte(uint8_t byte);

/**
 * @brief  向 TM1650 写入地址和数据
 * @return 0 成功, 1 失败
 */
uint8_t TM1650_Write(uint8_t addr, uint8_t data);

void tm1650_init(void);

// 使用示例：在第一个位置显示数字 5

/**
 * @brief 设置亮度
 * @param brightness 1-7 (1最暗, 7最亮, 0是自动/8级)
 */
void TM1650_SetBrightness(uint8_t brightness);

void TM1650_SetBrightness_Adaptive(uint8_t input_val);

/**
 * @brief 自动格式化显示时间
 * @param total_seconds 总秒数
 */
// todo,高位消隐会有bug,如果高位不为0, 低位是0,怎么办
void Display_Time_Auto(uint32_t total_seconds);