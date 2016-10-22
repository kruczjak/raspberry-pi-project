#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT 80
#define BACKLOG 10 // queue
#define SERVER_NAME "Server: kruczjak server"

#define ERROR {printf("FATAL (line %d): %s\n", __LINE__, strerror(errno)); \
        exit(errno);}


void sigchld_handler( int s )
{
    while( wait( NULL ) > 0 );

}

int main( int argc, char * argv[] )
{
    FILE * plik;

    int sockfd, new_fd; // nasluchiwanie na sockfd na nowe polaczenie

    struct sockaddr_in my_addr; // moj adres
    struct sockaddr_in their_addr; // adres clienta
    struct sigaction sa;

    socklen_t sin_size;
    int yes = 1, i = 0;
    char dane[ 999999 ]; //tablica znaków przechowuje plik index html

    if(( sockfd = socket( AF_INET, SOCK_STREAM, 0 ) ) == - 1 ) ERROR;
    if( setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, & yes, sizeof( int ) ) == - 1 ) ERROR;

    my_addr.sin_family = AF_INET; // rodzaj gniazda z ktorego kozysta TCP/IP
    my_addr.sin_port = htons( PORT ); // numer portu
    my_addr.sin_addr.s_addr = inet_addr( "0.0.0.0" ); // moje IP
    memset( &( my_addr.sin_zero ), '\0', 8 ); // zerowanie reszty struktury

    if( bind( sockfd,( struct sockaddr * ) & my_addr, sizeof( struct sockaddr ) ) == - 1 ) ERROR;
    if( listen( sockfd, BACKLOG ) == - 1 ) ERROR;


    sa.sa_handler = sigchld_handler; // zbieranie martwych procesow
    sigemptyset( & sa.sa_mask );
    sa.sa_flags = SA_RESTART;

    if( sigaction( SIGCHLD, & sa, NULL ) == - 1 ) ERROR;

    if ((plik = fopen( "index.html", "r" )) == NULL) ERROR;

    while( feof( plik ) == 0 ) {
        fscanf( plik, "%c", & dane[i] );
        i++;
    }

    fclose( plik ); // zamkniecie pliku

    while( 1 ) // głowna petla
    {
        sin_size = sizeof( struct sockaddr_in );

        if(( new_fd = accept( sockfd,( struct sockaddr * ) & their_addr, & sin_size ) ) == - 1 )
        {
            perror( "accept" );
            continue;
        }

        printf( "server: polaczono z %s\n", inet_ntoa( their_addr.sin_addr ) );

        pid_t child;

        if ((child = fork()) < 0) ERROR;

        if( child == 0 )
        { // to jest proces-dziecko
            close( sockfd ); // dziecko nie potrzebuje gniazda nasłuchujacego

            if( send( new_fd, dane, strlen( dane ), 0 == - 1 ) )
                 perror( "send" );

            close( new_fd );
            exit( 0 );
        }

    }
    return 0;
}

void print_base_headers(int client) {
    char buf[64];
    // HTTP version header
    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    // server name
    strcpy(buf, SERVER_NAME);
    send(client, buf, strlen(buf), 0);
    //content type
    strcpy(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

void send_file(int client, const char *filename) {
    FILE *resource = NULL;

    resource = fopen(filename, "r");
    print_base_headers(client);
    cat_to_socket(client, resource);
    fclose(resource);
}