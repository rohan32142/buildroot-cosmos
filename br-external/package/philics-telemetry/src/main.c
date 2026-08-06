#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#define OCM_BASE_ADDR   0xFFFFF000
#define MAP_SIZE        4096UL
#define MAP_MASK        (MAP_SIZE - 1)
#define DEFAULT_PORT    5005

int main(int argc, char *argv[]) {
    int fd;
    void *map_base, *virt_addr;
    int sockfd;
    struct sockaddr_in server_addr;

    if (argc < 2) {
        printf("Usage: philics-telemetry <TARGET_IP> [PORT]\n");
        return 1;
    }

    const char *target_ip = argv[1];
    int target_port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(target_port);
    server_addr.sin_addr.s_addr = inet_addr(target_ip);

    if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
        perror("Error opening /dev/mem");
        close(sockfd);
        return 1;
    }

    map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, OCM_BASE_ADDR & ~MAP_MASK);
    if (map_base == (void *) -1) {
        perror("Error mapping OCM memory");
        close(fd);
        close(sockfd);
        return 1;
    }

    printf("PHiLICS Telemetry Service started.\nStreaming 0x%X to %s:%d...\n", 
           OCM_BASE_ADDR, target_ip, target_port);

    while (1) {
        virt_addr = map_base + (OCM_BASE_ADDR & MAP_MASK);

        int16_t I_F = *((int16_t *)(virt_addr + 0x00));
        int16_t I_L = *((int16_t *)(virt_addr + 0x02));
        int16_t V_C = *((int16_t *)(virt_addr + 0x04));
        int16_t V_R = *((int16_t *)(virt_addr + 0x06));

        char payload[128];
        int len = snprintf(payload, sizeof(payload), "%d,%d,%d,%d\n", I_F, I_L, V_C, V_R);

        sendto(sockfd, payload, len, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

        usleep(20000); // 20 ms rate (50 Hz)
    }

    munmap(map_base, MAP_SIZE);
    close(fd);
    close(sockfd);
    return 0;
}