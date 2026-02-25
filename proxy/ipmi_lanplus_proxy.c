#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define LISTEN_PORT 623
#define TARGET_PORT 9001
#define TARGET_IP "127.0.0.1"
#define BUF_SIZE 2048

void hex_dump(const char *label, unsigned char *buf, int len) {
    printf("%s (%d bytes):\n", label, len);
    for (int i = 0; i < len; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n\n");
}

#include <stdint.h>

// This matches the "IPMB-like" structure inside the LAN packet
struct ipmi_lan_payload {
    uint8_t rs_sa;       // Responder Address (e.g., 0x20)
    uint8_t netfn_lun;   // Network Function and LUN
    uint8_t checksum1;   // Header checksum
    uint8_t rq_sa;       // Requester Address
    uint8_t rq_seq_lun;  // Sequence number
    uint8_t cmd;         // The actual IPMI Command
    uint8_t data[0];     // Start of the payload data
} __attribute__((packed));

void decode_ipmi_packet(unsigned char *buffer, int len) {
    // RMCP+ packets usually have the IPMI payload starting at offset 14
    // Note: This offset can vary if there is session authentication (RAKP)
    if (len < 20) return; 

    struct ipmi_lan_payload *ipmi = (struct ipmi_lan_payload *)&buffer[14];

    uint8_t netfn = ipmi->netfn_lun >> 2;
    uint8_t cmd = ipmi->cmd;

    printf("--- Decoded IPMI Layer ---\n");
    printf("NetFn: 0x%02x (", netfn);
    
    // Simple lookup for common NetFns
    switch(netfn) {
        case 0x06: printf("App"); break;
        case 0x07: printf("App Response"); break;
        case 0x0a: printf("Storage"); break;
        default: printf("Other"); break;
    }
    printf(")\n");

    printf("Command: 0x%02x\n", cmd);
    
    // If it's a response (NetFn is odd), the first byte of data is the Completion Code
    if (netfn % 2 != 0) {
        printf("Completion Code: 0x%02x\n", ipmi->data[0]);
    }
    printf("--------------------------\n\n");
}

int main() {
    int frontend_fd, backend_fd;
    struct sockaddr_in servaddr, cliaddr, targetaddr;
    unsigned char buffer[BUF_SIZE];
    socklen_t len;

    // 1. Setup Frontend Socket (Listening for ipmitool)
    if ((frontend_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Frontend socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(LISTEN_PORT);

    if (bind(frontend_fd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed. Try running with sudo (port 623)");
        exit(EXIT_FAILURE);
    }

    // 2. Setup Backend Socket (To talk to ipmi_sim)
    if ((backend_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Backend socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&targetaddr, 0, sizeof(targetaddr));
    targetaddr.sin_family = AF_INET;
    targetaddr.sin_port = htons(TARGET_PORT);
    inet_pton(AF_INET, TARGET_IP, &targetaddr.sin_addr);

    printf("IPMI Proxy started on port %d -> Forwarding to %s:%d\n", 
            LISTEN_PORT, TARGET_IP, TARGET_PORT);

    while (1) {
        len = sizeof(cliaddr);
        
        // Receive from ipmitool
        int n = recvfrom(frontend_fd, buffer, BUF_SIZE, 0, 
                         (struct sockaddr *)&cliaddr, &len);
        
        if (n > 0) {
            decode_ipmi_packet(buffer, n);
            hex_dump("From ipmitool", buffer, n);

            // Forward to ipmi_sim
            sendto(backend_fd, buffer, n, 0, 
                   (const struct sockaddr *)&targetaddr, sizeof(targetaddr));

            // Receive response from ipmi_sim
            struct sockaddr_in from_target;
            socklen_t target_len = sizeof(from_target);
            int resp_n = recvfrom(backend_fd, buffer, BUF_SIZE, 0, 
                                 (struct sockaddr *)&from_target, &target_len);

            if (resp_n > 0) {
                hex_dump("From ipmi_sim", buffer, resp_n);

                // Send back to ipmitool
                sendto(frontend_fd, buffer, resp_n, 0, 
                       (const struct sockaddr *)&cliaddr, len);
            }
        }
    }

    return 0;
}