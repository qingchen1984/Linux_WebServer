/* web_server_select.c */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

#define BUFSIZE 1024
#define MAX_QUE_CONN_NM 10

typedef struct {
	int maxfd;
	int maxi;
	int nready;
	int clientfd[FD_SETSIZE];
	int len[FD_SETSIZE];
	char request[FD_SETSIZE][BUFSIZE];
	fd_set read_set;
	fd_set ready_set;
} pool;

int start_server(int portnum);
void init_pool(int sockfd, pool *p);
void add_client(int connfd, pool *p);
void check_clients(pool *p);

int process_request(char *rq, int fd);
int resp_cannot_do(int fd);
int not_exist(char *f);
int resp_do_404(char *item, int fd);
int is_a_dir(char *f);
int resp_do_ls(char *dir, int fd);
int is_a_cgi_file(char *f);
int resp_do_exec(char *prog, int fd);
int resp_do_cat(char *f, int fd);
void pack_header(FILE *fp, char *extension);
char *get_filename_extension(char *f);

int main(int argc, char *argv[])
{
	int sock;
	int fd;
	int portnum;
	static pool pool;
	
	if (argc != 2) {
		fprintf(stderr, "usage: %s <portnum>\n", argv[0]);
		exit(-1);
	}
	portnum = atoi(argv[1]);
	sock = start_server(portnum);
	
	init_pool(sock, &pool);

	while(1) {
		pool.ready_set = pool.read_set;
		pool.nready = select(pool.maxfd + 1, &pool.ready_set, NULL, NULL, NULL);
		if ( pool.nready== -1) {
			perror("select");
			exit(-1);
		}

		if (FD_ISSET(sock, &pool.ready_set)) {
			if ((fd = accept(sock, NULL, NULL)) == -1) {
				perror("accept");
				close(sock);
				exit(-1);
			}
			add_client(fd, &pool);
		}
		
		check_clients(&pool);
	}
}

int start_server(int portnum)
{
	struct sockaddr_in server_sockaddr;
	int sockfd;
	int i = 1;

	/* create socket link */
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		exit(-1);
	}
	
	server_sockaddr.sin_family = AF_INET;
	server_sockaddr.sin_port = htons(portnum);
	server_sockaddr.sin_addr.s_addr = INADDR_ANY;
	bzero(server_sockaddr.sin_zero, 8);

	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
	
	if (bind(sockfd, (struct sockaddr *)&server_sockaddr, sizeof(struct sockaddr)) == -1) {
		perror("bind");
		exit(-1);
	}
	
	if (listen(sockfd, MAX_QUE_CONN_NM) == -1)
	{
		perror("listen");
		exit(-1);
	}
	
	return sockfd;
}

void init_pool(int sockfd, pool *p)
{
	int i;
	p->maxi = -1;
	for(i = 0; i < FD_SETSIZE; i++)
		p->clientfd[i] = -1;

	p->maxfd = sockfd;
	FD_ZERO(&p->read_set);
	FD_SET(sockfd, &p->read_set);
}

void add_client(int connfd, pool *p)
{
	int i;
	p->nready--;
	for (i = 0; i < FD_SETSIZE; i++)
		if (p->clientfd[i] < 0) {
			p->clientfd[i] = connfd;
			memset(p->request[i], 0, BUFSIZE);
			p->len[i] = 0;

			FD_SET(connfd, &p->read_set);

			if (connfd > p->maxfd)
				p->maxfd = connfd;
			if (i > p->maxi)
				p->maxi = i;

			break;
		}
		if (i == FD_SETSIZE)
			printf("add_client error: Too many clients\n");
}

void check_clients(pool *p)
{
	int i;
	int connfd;
	int n;
	int ret;
	char buf[BUFSIZE];
	char *q;

	for (i=0; (i<=p->maxi)&&(p->nready>0); i++) {
		connfd = p->clientfd[i];

		if ((connfd > 0) && (FD_ISSET(connfd, &p->ready_set))) {
			p->nready--;
			ret = read(connfd, p->request[i]+p->len[i], BUFSIZE);
			if (ret == -1) {
				perror("read error");
				close(connfd);
				continue;
			} else if (ret == 0) {
				printf("close\n");
				if (close(connfd) < 0) {
					perror("close connfd failed");
					exit(-1);
				}
				FD_CLR(connfd, &p->read_set);
				p->clientfd[i] = -1;
			} else {
				p->len[i] += ret;
				q = &p->request[i][p->len[i]-4];
				if (strcmp(q, "\r\n\r\n") == 0) {  //request[i] end
					q = strchr(p->request[i], '\r');
					*q = '\0';
					process_request(p->request[i], connfd);
					close(connfd);
					FD_CLR(connfd, &p->read_set);
					p->clientfd[i] = -1;
				}//if (strcmp(q, "\r\n\r\n") == 0)
			}//ret
		}//if((connfd > 0) && (FD_ISSET(connfd, &p->ready_set)))
	}//for
}

int process_request(char *rq, int fd)
{
	char cmd[BUFSIZE], arg[BUFSIZE];
	
	strcpy(arg, "./");
	if (sscanf(rq, "%s %s", cmd, arg + 2) != 2) {
		return 1;
	}
	
	if (strcmp(cmd, "GET") != 0)
	{
		resp_cannot_do(fd);
	} else if (not_exist(arg)) {
		resp_do_404(arg, fd);
	} else if (is_a_dir(arg)) {
		resp_do_ls(arg, fd);
	} else if (is_a_cgi_file(arg)) {
		resp_do_exec(arg, fd);
	} else {
		resp_do_cat(arg, fd);
	}
	
	return 0;
}

int resp_cannot_do(int fd)
{
	FILE *fp = fdopen(fd, "w");
	fprintf(fp, "HTTP/1.1 501 Not Implemented\r\n");
	fprintf(fp, "Content-type: text/plain\r\n");
	fprintf(fp, "\r\n");
	fprintf(fp, "That command is not yet implemented\r\n");
	fclose(fp);
	return 0;
}

int not_exist(char *f)
{
	struct stat info;
	return (stat(f, &info) == -1);
}

int resp_do_404(char *item, int fd)
{
	FILE *fp = fdopen(fd, "w");
	fprintf(fp, "HTTP/1.1 404 Not Found\r\n");
	fprintf(fp, "Content-type: text/plain\r\n");
	fprintf(fp, "\r\n");
	fprintf(fp, "The item you requested: %s\r\nis not found\r\n", item);
	fclose(fp);
	return 0;
}

int is_a_dir(char *f)
{
	struct stat info;
	return (stat(f, &info) != -1 && S_ISDIR(info.st_mode));
}

int resp_do_ls(char *dir, int fd)
{
	pid_t pid;
	FILE *fp = fdopen(fd, "w");

	pack_header(fp, "text/plain");
	fprintf(fp, "\r\n");
	fflush(fp);

	while ((pid = fork()) == -1);
	if (pid == 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);

		execlp("ls", "ls", "-l", dir, NULL);
		perror(dir);
		exit(-1);
	}

	waitpid(pid, NULL, 0);
	return 0;
}

int is_a_cgi_file(char *f)
{
	return(strcmp(get_filename_extension(f), "cgi") == 0);	
}

int resp_do_exec(char *prog, int fd)
{
	pid_t pid;
	FILE *fp = fdopen(fd, "w");

	pack_header(fp, NULL);
	fflush(fp);

	while ((pid = fork()) == -1);	
	if (pid == 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);

		execl(prog, prog, NULL);
		perror(prog);
		exit(-1);
	}

	waitpid(pid, NULL, 0);
	return 0;
}

int resp_do_cat(char *f, int fd)
{
	int c;
	char *extension = get_filename_extension(f);
	char *content = "text/plain";
	FILE *fpsock, *fpfile;

	if (strcmp(extension, "html") == 0) {
		content = "text/html";
	} else if (strcmp(extension, "gif") == 0) {
		content = "text/gif";
	} else if (strcmp(extension, "jpg") == 0) {
		content = "text/jpg";
	} else if (strcmp(extension, "jpeg") == 0) {
		content = "text/jpeg";
	}
	
	fpsock = fdopen(fd, "w");
	fpfile = fopen(f, "r");
	if (fpsock != NULL && fpfile != NULL) {
		pack_header(fpsock, content);
		fprintf(fpsock, "\r\n");
		
		while ((c = getc(fpfile)) != EOF) {
			putc(c, fpsock);
		}
		fclose(fpfile);
		fclose(fpsock);
		return 0;
	}
	return 1;
}

void pack_header(FILE *fp, char *extension)
{
	fprintf(fp, "HTTP/1.1 200 OK\r\n");
	if (extension)
		fprintf(fp, "Content-type: %s\r\n", extension);
}

char *get_filename_extension(char *f)
{
	return strrchr(f, '.') + 1; 
}
