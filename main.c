// Loader driver

#include "stm32ld.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include "cli.h"

static FILE *fp;
static u32 fpsize;

#define BL_VERSION_MAJOR  2
#define BL_VERSION_MINOR  1
#define BL_MKVER( major, minor )    ( ( major ) * 256 + ( minor ) ) 
#define BL_MINVERSION               BL_MKVER( BL_VERSION_MAJOR, BL_VERSION_MINOR )

// Supported CHIP_IDs
static const uint16_t SUPPORTED_CHIP_IDS[] =
{ 0x0410, 0x0414, 0x0413, 0x0440, 0 };

// ****************************************************************************
// Helper functions and macros

// Get data function
static u32 writeh_read_data(u8 *dst, u32 len)
{
    size_t readbytes = 0;

    if (!feof(fp))
        readbytes = fread(dst, 1, len, fp);
    return (u32) readbytes;
}

// Progress function
static void writeh_progress(u32 wrote)
{
    unsigned pwrite = (wrote * 100) / fpsize;
    static int expected_next = 10;

    if (pwrite >= expected_next)
    {
        printf("%d%% ", expected_next);
        expected_next += 10;
    }
}

// ****************************************************************************
// Entry point

int main(int argc, const char **argv)
{
    // Build parameter structure
    struct STM32PrgramParam
    {
        char port[256];
        char file[256];
        int baudrate;
        char skip_flash;
        char send_go;
    } ProgParam;

    // Set Initial Parameters
    ProgParam.baudrate = 9600;
    ProgParam.skip_flash = 0;
    ProgParam.send_go = 0;

    // Parse arguments to parameters
    stCliOption MainOpt[] =
    {
    { OPT_COMMENT, 0, NULL, "Usage Example: \n\t./stm32ld -p /dev/cu.usbmodem -f /path/to/file/flash.bin -g", NULL },
    { OPT_COMMENT, 0, NULL, "Essential Arguments: ", NULL },
    { OPT_STRING, 'p', "port", "UART port path, e.g. /dev/cu.usbmodem", (void*) &ProgParam.port },
    { OPT_STRING, 'f', "file", "Bin file path, e.g. /path/to/file/flash.bin", (void*) ProgParam.file },

    { OPT_COMMENT, 0, NULL, "Optional Arguments: ", NULL },
    { OPT_HELP, 'h', "help", "Show help hints", NULL },
    { OPT_INT, 'b', "baud", "Set UART baudrate, default is 9600.", (void*) &ProgParam.baudrate },
    { OPT_BOOL, 's', "skip", "Skip flash operation, only show device info.", (void*) &ProgParam.skip_flash },
    { OPT_BOOL, 'g', "go", "Send go command after flash finish, excute user program.", (void*) &ProgParam.send_go },
    { OPT_END, 0, NULL, NULL, NULL, NULL } };

    Cli_parseArgs(argc - 1, ++argv, MainOpt);

    printf("Port:[%s]\n", ProgParam.port);
    printf("File:[%s]\n", ProgParam.file);
    printf("Baud:[%d]\n", ProgParam.baudrate);
    printf("Skip:[%d]\n", ProgParam.skip_flash);
    printf("Go:[%d]\n", ProgParam.send_go);

    // Check Parameters
    if (ProgParam.port[0] == 0)
    {
        fprintf( stderr, "No UART port selected, try ./stm32ld -h\n");
        exit(1);
    }

    if (ProgParam.skip_flash == 0)
    {
        if (ProgParam.port[0] == 0)
        {
            fprintf( stderr, "No bin file selected, try ./stm32ld -h\n");
            exit(1);
        }
        // Check bin file
        if (strlen(ProgParam.file) == 1 && strncmp(ProgParam.file, "0", 1) == 0)
        {
            ProgParam.skip_flash = 1;
        }
        else
        {
            if ((fp = fopen(ProgParam.file, "rb")) == NULL)
            {
                fprintf( stderr, "Unable to open file %s\n", ProgParam.file);
                exit(1);
            }
            else
            {
                fseek(fp, 0, SEEK_END);
                fpsize = ftell(fp);
                fseek(fp, 0, SEEK_SET);
            }
        }
    }

    errno = 0;

    //1. Open UART and try connect to bootloader
    while (stm32_init(ProgParam.port, ProgParam.baudrate) != STM32_OK)
    {
        static int timeout = 0;
        timeout++;
        fprintf( stderr, "Sending 0x7F to STM32, no ACK got, retry = [%d]\n", timeout);
        sleep(1);
        if (timeout > 60)
        {
            fprintf( stderr, "Unable to connect to bootloader\n");
            exit(1);
        }
    }

    //2. Get version
    u8 minor, major;
    u16 version;

    if (stm32_get_version(&major, &minor) != STM32_OK)
    {
        fprintf( stderr, "Unable to get bootloader version\n");
        exit(1);
    }
    else
    {
        printf("Found bootloader version: %d.%d\n", major, minor);
        if ( BL_MKVER( major, minor ) < BL_MINVERSION)
        {
            fprintf( stderr, "Unsupported bootloader version\n");
            exit(1);
        }
    }

    //3. Get chip ID
    if (stm32_get_chip_id(&version) != STM32_OK)
    {
        fprintf( stderr, "Unable to get chip ID\n");
        exit(1);
    }
    else
    {
        const uint16_t *chip_ids = SUPPORTED_CHIP_IDS;
        printf("Chip ID: %04X\n", version);
        while (*chip_ids != 0)
        {
            if (*chip_ids == version)
            {
                break;
            }
            chip_ids++;
        }
        if (*chip_ids == 0)
        {
            fprintf( stderr, "Unsupported chip ID\n");
            exit(1);
        }
    }

    if (ProgParam.skip_flash == 0)
    {
        // Write unprotect
        if (stm32_write_unprotect() != STM32_OK)
        {
            fprintf( stderr, "Unable to execute write unprotect\n");
            exit(1);
        }
        else
            printf("Cleared write protection.\n");

        // Erase flash
        if (major == 3)
        {
            printf("Starting Extended Erase of FLASH memory. This will take some time ... Please be patient ...\n");
            if (stm32_extended_erase_flash() != STM32_OK)
            {
                fprintf( stderr, "Unable to extended erase chip\n");
                exit(1);
            }
            else
                printf("Extended Erased FLASH memory.\n");
        }
        else
        {
            if (stm32_erase_flash() != STM32_OK)
            {
                fprintf( stderr, "Unable to erase chip\n");
                exit(1);
            }
            else
                printf("Erased FLASH memory.\n");
        }

        // Program flash
        setbuf( stdout, NULL);
        printf("Programming flash ... ");
        if (stm32_write_flash(writeh_read_data, writeh_progress) != STM32_OK)
        {
            fprintf( stderr, "Unable to program FLASH memory.\n");
            exit(1);
        }
        else
            printf("\nDone.\n");

        fclose(fp);
    }
    else
        printf("Skipping flashing ... \n");

    if (ProgParam.send_go == 1)
    {
        // Run GO
        printf("Sending Go command ... \n");
        if (stm32_go_command() != STM32_OK)
        {
            fprintf( stderr, "Unable to run Go command.\n");
            exit(1);
        }
    }

    return 0;
}

