#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 50074
#endif
#define MAX_QUEUE 5

void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

int readTo(int soc, char* msg, int length){
	int n = 0;
	fprintf(stderr, "Read starting\n");
	if((n = read(soc, msg, length)) <= 0){
		//fprintf(stderr, "Read went wrong\n");
		//Client disconnected
		//remove_player(head, soc); We will remove_player on the outside
		fprintf(stderr, "Read zero. Socket Closed.\n");
		return -1;
	}
	fprintf(stderr, "Read complete\n");
	return n;
}

/* Wrapper write function with error checking
 *
 */
int writeTo(int soc, char* str, int length){
	int n = 0;
	if((n = write(soc, str, length)) <= 0){
		fprintf(stderr, "Write went wrong");
	}
	return n;
	
}

void broadcast(struct game_state *game, char *outbuf){
	for(struct client *r = game->head; r != NULL; r = r->next) {
		writeTo(r->fd, outbuf, strlen(outbuf));
	}
}
void start_new_game(struct game_state *game, char* dict){
	(*game).dict.fp = NULL;
	init_game(game, dict);
	broadcast(game, "\nStarting new game\n");
	char msg[MAX_MSG];
	status_message(msg, game);
	broadcast(game, msg);
	if(game->has_next_turn->next == NULL){
		game->has_next_turn = game->head;
	} else {
		game->has_next_turn = game->has_next_turn->next;
	}
	writeTo(game->has_next_turn->fd, "Your Guess?\n", 12);
}

/*
 * Search the first n characters of buf for a network newline (\r\n).
 * Returns the number of characters before the network newline
 * or -1 if no network newline is found.
 * or -1 if no network newline is found.
 */
int find_network_newline(const char *buf, int n) {
	for(int i = 0; i < n-1; i++){
		if(buf[i] == '\r' && buf[i+1] == '\n'){
			return i;
		}
	}
    return -1;
}

/*
 * Fully Read until a network newline is read, then return the length, like a read call.
 * Assumes MAX_MSG length for msg has been allocated
 */
int full_read(int cur_fd, char* msg){
	//char* clientbuf = malloc(MAX_MSG);
	//clientbuf[0] = '\0';
	int inbuf = 0;           // How many bytes currently in buffer?
	int room = MAX_MSG;  // How many bytes remaining in buffer?
	char *after = msg;   
	int numRead;
	while((numRead = readTo(cur_fd, after, room)) > 0){
		inbuf += numRead;
		room -= numRead;
		int where;
		if((where = find_network_newline(msg, inbuf)) > -1){
			//We found a network newline
			//*length = where;
			msg[where] = '\0'; //Getting rid of the newline and making it a string.
			return where;
		}
		//We haven't found a network newline, so we keep reading
		after = &msg[inbuf];
		room = MAX_MSG - inbuf;
		if(room == 0){ //For whatever reason the program reaches here, 
			return -1;
		}
	}
	return -1;
}


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}

int main(int argc, char **argv) {	
	//Piazza code to be added by A4 FAQ page
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	if(sigaction(SIGPIPE, &sa, NULL) == -1) {
		perror("sigaction");
		exit(1);
	}
	
	int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;
	
	

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, p->fd);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
		 
				
		/*NOTE FOR MARKER: 
		Some of the code has a bit repeated, but in doing so
		it remains the logical structure it has. More efficient
		if statements might make it harder for you to read so here you go
		
		Moreover, I did not choose to kick the player if write returned -1
		because in my testing, write sometimes *doesn't* return -1 when it writes
		to a broken socket. Read however, will know to immediately return 0, so I
		based off kicking players off of that. Lab computer agrees with me
		*/
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
		for(p = game.head; p != NULL; p = p->next) {
			if(cur_fd == p->fd) {
				char buf[MAX_MSG]; //Player entered
				char broad[MAX_MSG]; //Broadcast message
				char msg[MAX_MSG]; //Other messages
				int n;
				int guessed = 0;
				n = full_read(cur_fd, buf);
				if(n == -1){ //Client disconnected
					if(game.has_next_turn->fd == cur_fd){
						
						//Try to move the turn on if he's the current turn player
						if(game.has_next_turn == game.head && game.has_next_turn->next == NULL){
							//If he is the only player left
							game.has_next_turn = NULL;
						} else if(game.has_next_turn->next == NULL){
							//If he isn't the only player left but it's the end of the list
							game.has_next_turn = game.head;
							writeTo(game.has_next_turn->fd, "Your Guess?\n", 12);
						} else {
							game.has_next_turn = game.has_next_turn->next;
							writeTo(game.has_next_turn->fd, "Your Guess?\n", 12);
						}
					}
					strcpy(msg, p->name);
					strcat(msg, " has left the game\n");
					printf("%s left the game\n", p->name);
					remove_player(&game.head, cur_fd);
					broadcast(&game, msg);
					break;
				}
				buf[n] = '\0';
				printf("Pipe: [%d], Length of message: %d. Read the message: {%s}\n", cur_fd, n, buf);
				if(game.has_next_turn->fd != cur_fd){
					//Client wrote something not on his/her turn
					writeTo(cur_fd, "It's not your turn!\n", 19);
					break;
				}
				if(n != 1 || buf[0] < 'a' || buf[0] > 'z'){
					//Client wrote invalid guess
					writeTo(cur_fd, "Invalid Guess. Try again: ", 26);
					break;
				}
				if(game.letters_guessed[(int) (buf[0] - 'a')] == 1){
					//Client guessed a letter already guessed
					writeTo(cur_fd, "Already Guessed! Try again: ", 28);
					break;
				} else {
					//Client guessed a nice letter
					strcpy(msg, p->name);
					strcat(msg, " guessed ");
					strcat(msg, buf);
					strcat(msg, "\n");
					broadcast(&game, msg);
					game.letters_guessed[(int) (buf[0] - 'a')] = 1;
					for(int i = 0; i < MAX_WORD; i++){
						if(game.word[i] == buf[0]){
							game.guess[i] = buf[0];
							guessed = 1;
						}
					}
					if(guessed == 0){
						game.guesses_left -= 1;
						strcpy(msg, buf);
						strcat(msg, " is not in the word\n");
						broadcast(&game, msg);
					} else {
						broadcast(&game, "Nice Guess!\n");
					}
				}
				//Uncomment this block if you want to know the word
				// writeTo(cur_fd, "\nThe word: ", 11);
				// writeTo(cur_fd, game.word, strlen(game.word));
				// writeTo(cur_fd, "\n", 1);
				
				status_message(broad, &game);
				broadcast(&game, broad);
				//Check for winning
				int end = 0;
				for(int i = 0; i < strlen(game.word); i++){
					if(game.guess[i] == '-'){
						break;
					}
					if(i == strlen(game.word) - 1){ //The entire word is revealed
						broadcast(&game, "EVERYONE WINS\n");
						start_new_game(&game, argv[1]);
						end = 1;
					}
				}
				if(end == 1){
					break;
				}
				//Check for losing
				if(game.guesses_left == 0){
					broadcast(&game, "YOU ALL LOSERS\n");
					start_new_game(&game, argv[1]);
					break;
				}
				//Prompt a 'your guess' to the same client
				if(guessed == 1){
					writeTo(cur_fd, "Your Guess?\n", 12);
					break;
				}				
				//Otherwise, move the turn on and prompt the next one.
				game.has_next_turn = game.has_next_turn->next;

				if(game.has_next_turn == NULL){
					game.has_next_turn = game.head;
				}
				writeTo(game.has_next_turn->fd, "Your Guess?\n", 12);
				break;
			}
		}

		// Check if any new players are entering their names
		for(p = new_players; p != NULL; p = p->next) {
			if(cur_fd == p->fd) {
				// TODO - handle input from an new client who has
				// not entered an acceptable name.
				//read(cur_fd, after, room)
				
				char buf[MAX_MSG];
				int n;
				
				n = full_read(cur_fd, buf);
				if(n == -1){
					remove_player(&new_players, cur_fd);
					break;
				}
				buf[n] = '\0';
				printf("Pipe: [%d], Length of message: %d. Read the message: {%s}\n", cur_fd, n, buf);
				if(n <= 0){
					writeTo(cur_fd, "Name is blank. Please re-enter your name:", 41);
					break;
				}
				//Checking if name is taken
				int condition = 0;
				for(struct client *q = game.head; q!=NULL; q = q->next){
					if(strcmp(buf, q->name) == 0){
						writeTo(cur_fd, "Name is taken. Please re-enter your name:", 41);
						condition = 1;
						break;
					}
				}
				if(condition == 1){ //I didn't want to use goto, so here's another way
					break;
				}
				//Name is not taken
				strcpy(p->name, buf);
				if(p == new_players){
					new_players = new_players->next;
				} else {
					struct client *prev = new_players;
					for(struct client *q = new_players; q!=NULL; q = q->next){
						if(q->next == p){
							prev = q;
							break;
						}
					}
					prev->next = p->next;
				}
				p->next = game.head;
				game.head = p;
				
				//Tell everyone a new guy joined the game
				char broad[MAX_MSG];
				strcpy(broad, p->name);
				strcat(broad, " has joined the game\n");
				broadcast(&game, broad);
				printf("%s joined the game\n", p->name);
				
				//Tell the new guy the current status of the game
				char msg[MAX_MSG];
				status_message(msg, &game);
				writeTo(cur_fd, msg, strlen(msg));
				
				//Check if he's the first guy to join
				if(game.has_next_turn == NULL){
					game.has_next_turn = game.head;
					writeTo(cur_fd, "Your Guess?\n", 12);
				}
				break;
			} 
		}
            }
        }
    }
    return 0;
}


