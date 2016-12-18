#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>

#define PORT 80
#define BACKLOG_SIZE 10 // queue
#define END_OF_LINE '\0'
#define SERVER_NAME "Server: kruczjak server\r\n"

#define ERROR {printf("FATAL (line %d): %s\n", __LINE__, strerror(errno)); exit(errno);}

/* ############################################################ */
#define BCM2835_PERI_BASE       0x3F000000
#define GPIO_BASE               (BCM2835_PERI_BASE + 0x200000)
#define BLOCK_SIZE 		        (4*1024)

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x)
#define INP_GPIO(g)   *(gpio.addr + ((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g)   *(gpio.addr + ((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio.addr + (((g)/10))) |= (((a)<=3?(a) + 4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET  *(gpio.addr + 7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR  *(gpio.addr + 10) // clears bits which are 1 ignores bits which are 0

#define GPIO_READ(g)  *(gpio.addr + 13) &= (1<<(g))

// payload
#define LED_RED 16 // 0/1
#define LED_GREEN 20 // 0/1
#define LED_BLUE 21 // 0/1
// end of payload

#define sda 2
#define scl 3
#define I2C_DELAY 100

void wait_i2c_done();

struct bcm2835_peripheral {
    off_t addr_p;
    int mem_fd;
    void *map;
    volatile unsigned int *addr;
};

struct bcm2835_peripheral gpio = {GPIO_BASE};

int map_peripheral(struct bcm2835_peripheral *p)
{
    // Open /dev/mem
    if ((p->mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
        printf("Failed to open /dev/mem, try checking permissions.\n");
        return -1;
    }

    p->map = mmap(
            NULL,
            BLOCK_SIZE,
            PROT_READ|PROT_WRITE,
            MAP_SHARED,
            p->mem_fd,      // File descriptor to physical memory virtual file '/dev/mem'
            p->addr_p       // Address in physical map that we want this memory block to expose
    );

    if (p->map == MAP_FAILED) {
        ERROR;
        return -1;
    }

    p->addr = (volatile unsigned int *) p->map;

    return 0;
}

void unmap_peripheral(struct bcm2835_peripheral *p) {
    munmap(p->map, BLOCK_SIZE);
    close(p->mem_fd);
}
/* ############################################################ */
// input state == 1
// output state == 0

void delay() {
    usleep(I2C_DELAY);
}

void send_start() {
    INP_GPIO(sda);
    delay();
    INP_GPIO(scl);
    delay();
    OUT_GPIO(sda);
    delay();
    OUT_GPIO(scl);
    delay();
}

void send_stop() {
    OUT_GPIO(sda);
    delay();
    INP_GPIO(scl);
    delay();
    INP_GPIO(sda);
    delay();
}

int clock_read(void) {
    printf("CLOCK_READ: reading\n");
    int level;
    INP_GPIO(scl);
    delay();
    while(GPIO_READ(scl) == 0);
    delay();
    level = GPIO_READ(sda);
    printf("CLOCK_READ: readed %d\n", level);
    delay();
    OUT_GPIO(scl);

    if (level > 1) level = 1;
    return(level);
}

int send_byte(int byte) {
    int mask = 0x80;
    while(mask) {
        if (byte & mask) {
            printf("SEND_BYTE: 1\n");
            INP_GPIO(sda);
        } else {
            printf("SEND_BYTE: 0\n");
            OUT_GPIO(sda);
        }
        clock_read();
        mask >>= 1;
    }
    INP_GPIO(sda);
    return(clock_read());
}

int read_byte(int acknowledgment)
{
    int mask = 0x80, byte = 0x00;
    while(mask) {
        if (clock_read()) byte |= mask;
        mask >>= 1;
    }
    if (acknowledgment) {
        OUT_GPIO(sda);
        clock_read();
        INP_GPIO(sda);
    } else {
        INP_GPIO(sda);
        clock_read();
    }
    return(byte);
}

// startuje i2c
void i2c_init() {
    OUT_GPIO(sda);
    OUT_GPIO(scl);
    GPIO_CLR = 1 << sda;
    GPIO_CLR = 1 << scl;

    send_start();
    printf("!!! FIRST ACK %d\n", send_byte(0x46));
    printf("!!! ACK %d\n", send_byte(0x10));
    send_stop();
}

int readLuxes() {
    send_start();
    printf("!!! ACK %d\n", send_byte(0x47));
    int highByte = read_byte(1);
    printf("!!! byte1: %d\n", highByte);
    int lowByte = read_byte(0);
    printf("!!! byte2: %d\n", lowByte);
    send_stop();

    int value = (highByte << 8) | lowByte;
    int lux = (int) (value / 1.2);
    printf("!!!luxes: %d\n", lux);

    return lux;
}
/* ############################################################ */

//definitions
int get_line_from_socket_to_buffer(int, char *, int);
void response_file_headers(int);
void cat_file_to_socket(int, FILE *);
void send_file(int client_socket, const char *filename);
void accept_client_request(int);
int start_socket_listening();
void process_post(int, char);
void initialize_ports();
void render_index(int);
void enable_port(unsigned int);
void disable_port(unsigned int);
void init_output(unsigned int);
void init_input(unsigned int);
void render_gpio(int);
void render_luxes(int);
void write_simple_headers(int);
char get_simple_state(unsigned int);

void sigchld_handler(int s) {
    while( wait( NULL ) > 0 );
}

void set_sigchld_trap() {
    struct sigaction sa;

    sa.sa_handler = sigchld_handler; // zbieranie martwych procesow
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if(sigaction( SIGCHLD, &sa, NULL ) == - 1) ERROR;
}

int get_line_from_socket_to_buffer(int socket, char *buffer, int size) {
    int i = 0;
    char readed_char = END_OF_LINE;
    ssize_t recv_status;

    while ((i < size - 1) && (readed_char != '\n')) {
        recv_status = recv(socket, &readed_char, 1, 0);

        if (recv_status <= 0) {
            readed_char = '\n';
            continue;
        }

        if (readed_char == '\r') {
            recv_status = recv(socket, &readed_char, 1, MSG_PEEK);

            if ((recv_status > 0) && (readed_char == '\n')) {
                recv(socket, &readed_char, 1, 0);
            } else {
                readed_char = '\n';
            }
        }

        buffer[i] = readed_char;
        i++;
    }

    buffer[i] = END_OF_LINE; // zakoncz linie

    return i;
}

void response_file_headers(int client_socket) {
    char buffer[1024];

    strcpy(buffer, "HTTP/1.0 200 OK\r\n");
    send(client_socket, buffer, strlen(buffer), 0);
    strcpy(buffer, SERVER_NAME);
    send(client_socket, buffer, strlen(buffer), 0);
    sprintf(buffer, "Content-Type: text/html; charset=UTF-8\r\n");
    send(client_socket, buffer, strlen(buffer), 0);
    strcpy(buffer, "\r\n");
    send(client_socket, buffer, strlen(buffer), 0);
}

void cat_file_to_socket(int client_socket, FILE *resource) {
    char buffer[1024];

    fgets(buffer, sizeof(buffer), resource);
    while (!feof(resource)) {
        send(client_socket, buffer, strlen(buffer), 0);
        fgets(buffer, sizeof(buffer), resource);
    }
}

void send_file(int client_socket, const char *filename) {
    FILE *resource = NULL;
    int number_of_chars = 1;
    char buffer[1024];

    buffer[0] = 'A'; buffer[1] = '\0';

    // porzuc naglowki
    while ((number_of_chars > 0) && strcmp("\n", buffer)) {
        number_of_chars = get_line_from_socket_to_buffer(client_socket, buffer, sizeof(buffer));
    }

    resource = fopen(filename, "r");
    response_file_headers(client_socket);
    cat_file_to_socket(client_socket, resource);
    fclose(resource);
}

void accept_client_request(int client) {
    char buffer[1024]; // tylko 1024!
    char request_type[255];
    char url[255];
    size_t position = 0, current_buffer_position = 0;

    char *query_string = NULL;

    get_line_from_socket_to_buffer(client, buffer, sizeof(buffer));

    // wczytaj typ requestu
    while (!isspace(buffer[position]) && (position < sizeof(request_type) - 1)) { // znak whitespace lub koniec miejsca
        request_type[position] = buffer[position];
        position++;
    }
    request_type[position] = END_OF_LINE;

    current_buffer_position = position;
    position = 0;

    // omin biale znaki
    while (isspace(buffer[current_buffer_position]) && (current_buffer_position < sizeof(buffer))) {
        current_buffer_position++;
    }

    // wczytaj url
    while (!isspace(buffer[current_buffer_position]) && (position < sizeof(url) - 1) && (current_buffer_position < sizeof(buffer))) {
        url[position] = buffer[current_buffer_position];
        position++;
        current_buffer_position++;
    }
    url[position] = END_OF_LINE;

    // query string
    query_string = url;
    while((*query_string != '?') && (*query_string != END_OF_LINE)) query_string++;

    printf("DEBUG: Request received: (%s) %s, query: (%s)\r\n", request_type, url, query_string);

    // GET przetwarzanie
    if (strcasecmp(request_type, "GET") == 0) {
        if (strcasecmp(query_string, "") == 0) return render_index(client);
        if (strcasecmp(query_string, "?leds") == 0) return render_gpio(client);
        if (strcasecmp(query_string, "?light") == 0) return render_luxes(client);
        // GET with query string

    }
    // POST przetwarzanie
    if (strcasecmp(request_type, "POST") == 0) {
        // POST!!!
        char buffer[1024];
        int content_length = -1;
        int number_of_chars;

        buffer[0] = 'A';
        buffer[1] = END_OF_LINE;

        // content-length
        number_of_chars = get_line_from_socket_to_buffer(client, buffer, sizeof(buffer));
        while ((number_of_chars > 0) && strcmp("\n", buffer)) {
            buffer[15] = '\0';
            if (strcasecmp(buffer, "Content-Length:") == 0) content_length = atoi(&(buffer[16]));
            number_of_chars = get_line_from_socket_to_buffer(client, buffer, sizeof(buffer));
        }

        printf("DEBUG: CONTENT_LENGTH=%d\r\n", content_length);

        char single_byte;

        for (int i = 0; i < content_length; i++) {
            recv(client, &single_byte, 1, 0);
            printf("DEBUG: POST_BODY[%d]=%c\r\n", i, single_byte);
            process_post(i, single_byte);
        }
        printf("END OF POST\r\n");

        response_file_headers(client);
    }

    fflush(stdout);
    close(client);
}

void render_index(int client) {
    printf("DEBUG: Rendering index.html\r\n");

    send_file(client, "index.html");
    fflush(stdout);
    close(client);
}

void render_gpio(int client) {
    printf("DEBUG: Rendering GPIO\r\n");
    char buffer[1024];

    buffer[0] = END_OF_LINE;

    printf("LED_RED state: %u\r\n", GPIO_READ(LED_RED));
    printf("LED_GREEN state: %u\r\n", GPIO_READ(LED_GREEN));
    printf("LED_BLUE state: %u\r\n", GPIO_READ(LED_BLUE));

    write_simple_headers(client);

    buffer[0] = get_simple_state(LED_RED);
    buffer[1] = END_OF_LINE;
    send(client, buffer, strlen(buffer), 0);

    buffer[0] = get_simple_state(LED_GREEN);
    buffer[1] = END_OF_LINE;
    send(client, buffer, strlen(buffer), 0);

    buffer[0] = get_simple_state(LED_BLUE);
    buffer[1] = END_OF_LINE;
    send(client, buffer, strlen(buffer), 0);

    strcpy(buffer, "\r\n");
    send(client, buffer, strlen(buffer), 0);

    fflush(stdout);
    close(client);
}

void render_luxes(int client) {
    printf("DEBUG: Rendering luxes\r\n");
    char buffer[1024];
    buffer[0] = END_OF_LINE;
    write_simple_headers(client);

    int luxes = readLuxes();
    char str[15];
    sprintf(str, "%d", luxes);
    strcpy(buffer, str);
    strcpy(buffer, "\r\n");
    send(client, buffer, strlen(buffer), 0);
    strcpy(buffer, "\r\n");
    send(client, buffer, strlen(buffer), 0);

    fflush(stdout);
    close(client);
}

void write_simple_headers(int client) {
    char buffer[1024];
    int number_of_chars = 1;
    buffer[0] = 'A';
    buffer[1] = END_OF_LINE;

    while ((number_of_chars > 0) && strcmp("\n", buffer)) {
        number_of_chars = get_line_from_socket_to_buffer(client, buffer, sizeof(buffer));
    }
    buffer[0] = END_OF_LINE;

    strcpy(buffer, "HTTP/1.0 200 OK\r\n");
    send(client, buffer, strlen(buffer), 0);
    strcpy(buffer, SERVER_NAME);
    send(client, buffer, strlen(buffer), 0);
    sprintf(buffer, "Content-Type: text/html; charset=UTF-8\r\n");
    send(client, buffer, strlen(buffer), 0);
    strcpy(buffer, "\r\n");
    send(client, buffer, strlen(buffer), 0);
}

char get_simple_state(unsigned int port_number) {
    unsigned int readed_value = GPIO_READ(port_number);

    if (readed_value == 0) return '0';
    return '1';
}

int start_socket_listening() {
    int httpd;
    struct sockaddr_in server_addr;

    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1) ERROR;

    memset(&server_addr, 0, sizeof(server_addr)); // zerowanie struktury
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(httpd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) ERROR;
    if (listen(httpd, BACKLOG_SIZE) < 0) ERROR;

    return httpd;
}

void initialize_ports() {
    init_output(LED_RED);
    init_output(LED_GREEN);
    init_output(LED_BLUE);
}

void enable_port(unsigned int port_number) {
    GPIO_SET = 1 << port_number;
}

void disable_port(unsigned int port_number) {
    GPIO_CLR = 1 << port_number;
}

void init_output(unsigned int port_number) {
    INP_GPIO(port_number);
    OUT_GPIO(port_number);
}

void init_input(unsigned int port_number) {
    INP_GPIO(port_number);
}

void process_post(int byte_number, char byte) {
    switch(byte_number) {
        case 0:
            if (byte == '0') enable_port(LED_RED);
            if (byte == '1') disable_port(LED_RED);
            return;
        case 1:
            if (byte == '0') enable_port(LED_GREEN);
            if (byte == '1') disable_port(LED_GREEN);
            return;
        case 2:
            if (byte == '0') enable_port(LED_BLUE);
            if (byte == '1') disable_port(LED_BLUE);
            return;
        default:
            printf(":/");
            return;
    }
}

int main( int argc, char * argv[] ) {
    int server_socket;
    int client_socket;
    pthread_t client_thread;

    map_peripheral(&gpio); // mapowanie GPIO
    initialize_ports();
    i2c_init();

    set_sigchld_trap();

    struct sockaddr_in client_addr; // adres clienta
    socklen_t client_addr_length = sizeof(client_addr);

    // starting server
    server_socket = start_socket_listening();
    printf("http server running on port %d\n", PORT);

    // starting client request accepting
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *) &client_addr, &client_addr_length);

        if (client_socket == -1) ERROR;
        if (pthread_create(&client_thread, NULL, (void *(*)(void *)) accept_client_request, (void *) client_socket) != 0) ERROR;
    }

    close(server_socket);
    unmap_peripheral(&gpio);

    return 0;
}
