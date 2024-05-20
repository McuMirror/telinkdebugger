#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/divider.h"

#include "sws.pio.h"

#define SWS_PIN 20
#define RST_PIN 21
#define LED_PIN PICO_DEFAULT_LED_PIN

#define SM_CLOCK 0
#define SM_DATA 1

#define BUFFER_SIZE_BITS 4096

#define REG_ADDR8(n) (n)
#define REG_ADDR16(n) (n)
#define REG_ADDR32(n) (n)

#define reg_soc_id REG_ADDR16(0x7e)

#define reg_swire_data REG_ADDR8(0xb0)
#define reg_swire_ctrl1 REG_ADDR8(0xb1)
#define reg_swire_clk_div REG_ADDR8(0xb2)
#define reg_swire_id REG_ADDR8(0xb3)

#define reg_tmr_ctl REG_ADDR32(0x620)
#define FLD_TMR_WD_EN (1 << 23)

#define reg_debug_runstate REG_ADDR8(0x602)

#define BITS_PER_RECEIVED_BYTE (10 * 10)

static uint32_t input_buffer[BUFFER_SIZE_BITS / 8];
static uint32_t output_buffer[BUFFER_SIZE_BITS / 8];

static int input_buffer_bit_ptr;
static int output_buffer_bit_ptr;

static int sws_program_offset;

static bool is_connected;

static void write_raw_bit(bool bit)
{
    if (output_buffer_bit_ptr < BUFFER_SIZE_BITS)
    {
        uint32_t* p = &output_buffer[output_buffer_bit_ptr / 32];
        uint32_t mask = 0x80000000 >> (output_buffer_bit_ptr & 31);
        if (bit)
            *p |= mask;
        else
            *p &= ~mask;
        output_buffer_bit_ptr++;
    }
}

static void write_raw_bits(bool bit, int count)
{
    while (count--)
        write_raw_bit(bit);
}

static void write_baked_bit(bool bit)
{
    if (bit)
    {
        write_raw_bits(0, 4);
        write_raw_bit(1);
    }
    else
    {
        write_raw_bit(0);
        write_raw_bits(1, 4);
    }
}

static void write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++)
    {
        write_baked_bit(byte & 0x80);
        byte <<= 1;
    }
    write_baked_bit(0);
}

static void write_cmd_byte(uint8_t byte)
{
    write_baked_bit(1);
    write_byte(byte);
}

static void write_data_byte(uint8_t byte)
{
    write_baked_bit(0);
    write_byte(byte);
}

static void write_data_word(uint16_t word)
{
    write_data_byte(word >> 8);
    write_data_byte(word & 0xff);
}

static bool read_raw_bit()
{
    if (input_buffer_bit_ptr < BUFFER_SIZE_BITS)
    {
        uint32_t* p = &input_buffer[input_buffer_bit_ptr / 32];
        uint32_t mask = 0x80000000 >> (input_buffer_bit_ptr & 31);
        input_buffer_bit_ptr++;

        return *p & mask;
    }

    return false;
}

static uint8_t decode()
{
    uint8_t result = 0;

    input_buffer_bit_ptr = 0;

    bool last_bit = read_raw_bit();
    while (last_bit && (input_buffer_bit_ptr != BUFFER_SIZE_BITS))
        last_bit = read_raw_bit();

    for (int i = 0; i < 8; i++)
    {
        int lowcount = 0;
        do
        {
            lowcount++;
            last_bit = read_raw_bit();
            if (input_buffer_bit_ptr == BUFFER_SIZE_BITS)
                return result;
        } while (!last_bit);

        int highcount = 0;
        do
        {
            highcount++;
            last_bit = read_raw_bit();
            if (input_buffer_bit_ptr == BUFFER_SIZE_BITS)
                return result;
        } while (last_bit);

        result = (result << 1) | (lowcount > highcount);

        if (input_buffer_bit_ptr == BUFFER_SIZE_BITS)
            break;
    }

    return result;
}

static uint8_t send_receive(int readcount)
{
    sws_program_init(pio0, SM_DATA, sws_program_offset, SWS_PIN);
    pio_sm_set_enabled(pio0, SM_DATA, true);

    pio_sm_put_blocking(
        pio0, SM_DATA, output_buffer_bit_ptr);     // bits to transmit
    pio_sm_put_blocking(pio0, SM_DATA, readcount); // bits to receive

    uint32_t* p = &output_buffer[0];
    while (!pio_interrupt_get(pio0, 1))
    {
        if (pio_sm_is_tx_fifo_empty(pio0, SM_DATA))
            pio_sm_put(pio0, SM_DATA, *p++);
    }

    p = &input_buffer[0];
    pio_interrupt_clear(pio0, 1);
    while (
        !pio_interrupt_get(pio0, 2) || !pio_sm_is_rx_fifo_empty(pio0, SM_DATA))
    {
        if (!pio_sm_is_rx_fifo_empty(pio0, SM_DATA))
            *p++ = pio_sm_get(pio0, SM_DATA);
    }

    pio_interrupt_clear(pio0, 2);
    if (readcount)
        return decode();
    else
        return 0;
}

static uint8_t read_first_debug_byte(uint16_t address)
{
    output_buffer_bit_ptr = 0;
    write_cmd_byte(0x5a);
    write_data_word(address);
    write_data_byte(0x80);
    write_raw_bits(0, 4);
    return send_receive(BITS_PER_RECEIVED_BYTE);
}

static uint8_t read_next_debug_byte()
{
    output_buffer_bit_ptr = 0;
    write_raw_bits(0, 4);
    return send_receive(BITS_PER_RECEIVED_BYTE);
}

static void finish_reading_debug_bytes()
{
    output_buffer_bit_ptr = 0;
    write_cmd_byte(0xff);
    send_receive(0);
}

static uint8_t read_single_debug_byte(uint16_t address)
{
    uint8_t value = read_first_debug_byte(address);
    finish_reading_debug_bytes();
    return value;
}

static uint16_t read_single_debug_word(uint16_t address)
{
    uint8_t v1 = read_first_debug_byte(address);
    uint8_t v2 = read_next_debug_byte();
    finish_reading_debug_bytes();
    return v1 | (v2 << 8);
}

static void write_first_debug_byte(uint16_t address, uint8_t value)
{
    output_buffer_bit_ptr = 0;
    write_cmd_byte(0x5a);
    write_data_word(address);
    write_data_byte(0x00);
    write_data_byte(value);

    send_receive(0);
}

static void write_next_debug_byte(uint8_t value)
{
    output_buffer_bit_ptr = 0;
    write_data_byte(value);

    send_receive(0);
}

static void finish_writing_debug_bytes()
{
    output_buffer_bit_ptr = 0;
    write_cmd_byte(0xff);

    send_receive(0);
}

static void write_single_debug_byte(uint16_t address, uint8_t value)
{
    write_first_debug_byte(address, value);
    finish_writing_debug_bytes();
}

static void write_single_debug_word(uint16_t address, uint16_t value)
{
    write_first_debug_byte(address, value);
    write_next_debug_byte(value >> 8);
    finish_writing_debug_bytes();
}

static void write_single_debug_quad(uint16_t address, uint32_t value)
{
    write_first_debug_byte(address, value);
    write_next_debug_byte(value >> 8);
    write_next_debug_byte(value >> 16);
    write_next_debug_byte(value >> 24);
    finish_writing_debug_bytes();
}

static void halt_target()
{
    write_single_debug_byte(reg_debug_runstate, 0x05);
}

static void set_target_clock_speed(uint8_t speed)
{
    write_single_debug_byte(reg_swire_clk_div, speed);
}

static void banner()
{
    printf("# Telink debugger bridge\n");
    printf("# (help placeholder text here)\n");
}

static void init_cmd()
{
    printf("# init\n");

    for (int speed = 3; speed < 0x7f; speed++)
    {
        gpio_put(RST_PIN, false);
        sleep_ms(20);
        gpio_put(RST_PIN, true);
        sleep_ms(20);

        halt_target();

        set_target_clock_speed(speed);

        uint16_t socid = read_single_debug_word(reg_soc_id);
        if (socid == 0x5316)
        {
            speed *= 2;
            printf("S\n# using speed %d\n", speed);
            set_target_clock_speed(speed);
            is_connected = true;

            /* Disable the watchdog timer. */

            write_single_debug_quad(reg_tmr_ctl, 0);

            return;
        }
    }

    printf("E\n# init failed\n");
}

static uint8_t read_hex_byte()
{
    char buffer[3];
    buffer[0] = getchar();
    buffer[1] = getchar();
    buffer[2] = 0;
    return strtoul(buffer, nullptr, 16);
}

static uint16_t read_hex_word()
{
    uint8_t hi = read_hex_byte();
    uint8_t lo = read_hex_byte();
    return lo | (hi << 8);
}

int main()
{
    stdio_init_all();

    gpio_init(RST_PIN);
    gpio_set_dir(RST_PIN, true);
    gpio_put(RST_PIN, false);

    gpio_set_pulls(RST_PIN, false, false);
    gpio_set_pulls(SWS_PIN, false, false);
    sleep_ms(100);

    sws_program_offset = pio_add_program(pio0, &sws_program);
    int timer_offset = pio_add_program(pio0, &timer_program);

    double ticks_per_second = clock_get_hz(clk_peri);
    double debug_clock_rate_hz = 0.250e-6;

    timer_program_init(
        pio0, SM_CLOCK, timer_offset, debug_clock_rate_hz * ticks_per_second);
    pio_sm_set_enabled(pio0, SM_CLOCK, true);

    for (;;)
    {
        static bool was_usb_connected = false;
        bool is_usb_connected = stdio_usb_connected();
        if (is_usb_connected && !was_usb_connected)
            banner();
        was_usb_connected = is_usb_connected;

        if (is_usb_connected)
        {
            int c = getchar();
            switch (c)
            {
                case 'i':
                    init_cmd();
                    break;

                case 'r':
                {
                    int i = getchar() == '1';
                    printf("# reset <- %d\n", i);
                    gpio_put(RST_PIN, i);
                    if (i == 0)
                        is_connected = false;
                    printf("S\n");
                    break;
                }

                case 's':
                {
                    uint16_t socid = read_single_debug_word(reg_soc_id);
                    printf("# socid = %04x\n", socid);
                    break;
                }

                case 'R':
                {
                    uint16_t address = read_hex_word();
                    uint16_t count = read_hex_word();

                    if (count)
                    {
                        uint8_t b = read_first_debug_byte(address);
                        printf("%02x", b);
                        count--;

                        while (count--)
                        {
                            b = read_next_debug_byte();
                            printf("%02x", b);
                        }

                        finish_reading_debug_bytes();
                        printf("\n");
                    }
                    printf("S\n");
                    break;
                }

                case 'W':
                {
                    uint16_t address = read_hex_word();
                    uint16_t count = read_hex_word();

                    if (count)
                    {
                        uint8_t b = read_hex_byte();
                        write_first_debug_byte(address, b);
                        count--;

                        while (count--)
                        {
                            b = read_hex_byte();
                            write_next_debug_byte(b);
                        }

                        finish_writing_debug_bytes();
                    }

                    printf("S\n");
                    break;
                }

                default:
                    printf("?\n");
                    printf("# unknown command\n");
            }
        }
    }

    halt_target();
}
