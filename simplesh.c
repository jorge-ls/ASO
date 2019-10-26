/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliacion de Sistemas Operativos
 * Departamento de Ingenieria y Tecnologia de Computadores
 * Facultad de Informatica de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (vease /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <limits.h>
#include <libgen.h>
#include <signal.h>
#include <math.h>



// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.19";

// Niveles de depuracion
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


//Número maximo de argumentos de un comando
#define MAX_ARGS 16
//Número de comandos internos
#define NUM_INTERNOS 5
//Tamaño minimo del buffer de lectura
#define MIN_BSIZE 1
//Tamaño maximo del bloque de lectura
#define MAX_BSIZE pow(2,20)
//Número maximo de comandos activos en segundo plano
#define MAX_BACK 8
//Array de comandos internos
const char * cmdInternos[NUM_INTERNOS] = {"cwd","cd","exit","psplit","bjobs"};
//Array de procesos activos en segundo plano
pid_t backcmds[MAX_BACK];
// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";

char pathAnterior[PATH_MAX];
int std_out;
int back_prcs = 0; //Número de procesos en segundo plano actual
sigset_t blocked_signals; // Señales bloqueadas en el proceso
//sigset_t old_blocked_signals;

/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecucion
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar informacion que servira a
// simplesh para ejecutar lineas de ordenes con redirecciones, tuberias, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Notese como las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de el se obtiene un tipo derivado a traves de
// *casting* forzado de tipo. Se consigue asi polimorfismo basico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parametros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redireccion
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tuberia
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de ordenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el analisis sintactico de la linea de ordenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char const* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *despues* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como paramero (`start_of_str`) avanza hasta el
// primer caracter que no esta en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo despues de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char const* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);


// `parse_cmd` realiza el *analisis sintactico* de la linea de ordenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la linea de ordenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintactico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el analisis sintactico de la linea de ordenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la linea contiene alguna tuberia.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de ordenes y/o redirecciones.  A continuacion, `parse_line`
// comprueba si la ejecucion de la linea se realiza en segundo plano (con `&`)
// o si la linea de ordenes contiene una lista de ordenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintactico: no se encontro comando\n", __func__);

        // Consume el delimitador de lista de ordenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el analisis sintactico de una tuberia de manera
// recursiva si encuentra el delimitador de tuberias '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el analisis sintactico de todos los componentes de la tuberia.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintactico: no se encontro comando\n", __func__);

        // Consume el delimitador de tuberia
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tuberia
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el analisis sintactico de un comando a no ser que la
// expresion comience por un parentesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y despues del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una linea de ordenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintactico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones despues del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene mas parametros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el analisis sintactico de un bloque de ordenes
// delimitadas por parentesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones despues del bloque de ordenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el parentesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintactico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el analisis sintactico hasta el parentesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de ordenes
    cmd = subscmd(scmd);

    // Consume el parentesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintactico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones despues del bloque de ordenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el analisis sintactico de ordenes con
// redirecciones si encuentra alguno de los delimitadores de
// redireccion ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuacion es delimitador de
    // redireccion...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redireccion
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redireccion entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintactico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redireccion
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, S_IRWXU, STDIN_FILENO);
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU, STDOUT_FILENO);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_APPEND, S_IRWXU, STDOUT_FILENO);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}

//Declaracion adelantada de funciones de comandos internos

void run_exit(struct execcmd * ecmd);
void run_cd(struct execcmd * ecmd);
void run_cwd();
void exec_cmdInterno(struct execcmd * ecmd);
int isInterno(struct execcmd * ecmd);
void run_psplit(struct execcmd * ecmd);
void auxPsplit(int numLineas,int numBytes,int bsize,int fd,char * nombreFichero);

/******************************************************************************
 * Funciones para la ejecucion de la linea de ordenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontro el comando '%s'\n", ecmd->argv[0]);
}

//Funcion que añade una señal a la mascara de bloqueo de señales del proceso
void bloquearSenal(int sig){
	if (sigaddset(&blocked_signals,sig) < 0){
		perror("bloquearSenal: sigaddset");
        	exit(EXIT_FAILURE);
    	}
	if (sigprocmask(SIG_SETMASK,&blocked_signals,NULL) < 0){ 
		perror("bloquearSenal: sigprocmask");
        	exit(EXIT_FAILURE);
	}

}
//Funcion que elimina una señal de la mascara de bloqueo de señales del proceso
void desbloquearSenal(int sig){
	if (sigdelset(&blocked_signals,sig) < 0){
		perror("desbloquearSenal: sigdelset");
        	exit(EXIT_FAILURE);
    	}
	if (sigprocmask(SIG_SETMASK,&blocked_signals,NULL) < 0){ 
		perror("desbloquearSenal: sigprocmask");
        	exit(EXIT_FAILURE);
	}

}
void run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;
    int status;
    pid_t pidChild;
    pid_t leftChild;
    pid_t rightChild;
    pid_t subsChild;
    pid_t redrChild;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            	ecmd = (struct execcmd*) cmd;
	    	if (isInterno(ecmd)) //Caso en el que se ejecuta un comando interno 
			exec_cmdInterno(ecmd);
		else{ //Comando externo
			bloquearSenal(SIGCHLD); //Se bloquea la señal SIGCHLD en cada caso para que no se ejecute el waitpid del manejador
        	    	if ((pidChild = fork_or_panic("fork EXEC")) == 0){
				exec_cmd(ecmd);
			}
			//TRY( wait(NULL) );
			if (waitpid(pidChild,&status,0) < 0){
				perror("run_cmd: exec waitpid");
				exit(EXIT_FAILURE);
			}
			desbloquearSenal(SIGCHLD); //Cuando se ejecuta el waitpid anterior se desbloquea la señal
		}

            	break;

        case REDR: 
            rcmd = (struct redrcmd*) cmd;
 	    if (isInterno((struct execcmd*) rcmd->cmd)){
		if ((std_out = dup(1)) == -1){ //Se crea una copia de stdout 
		    perror("run_cmd: dup");
                    exit(EXIT_FAILURE);
		}
		TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags,rcmd->mode)) < 0)
                {
                    perror("run_cmd: open");
                    exit(EXIT_FAILURE);
                }
	    	if (rcmd->cmd->type == EXEC)
			exec_cmdInterno((struct execcmd*) rcmd->cmd);
		else
			run_cmd(rcmd->cmd);
		//Se cierra el fichero creado para la redireccion
		if (close(fd) == -1){
		    perror("run_cmd: close");
                    exit(EXIT_FAILURE);
		}
		//Se vuelve a abrir la salida estandar
		if (dup2(std_out,1) == -1){
			perror("run_cmd: dup2");
			exit(EXIT_FAILURE);
		}
	    } else {
		bloquearSenal(SIGCHLD);
		if ((redrChild = fork_or_panic("fork REDR")) == 0){
                	TRY( close(rcmd->fd) );
                	if ((fd = open(rcmd->file, rcmd->flags,rcmd->mode)) < 0){
                    		perror("run_cmd: open");
                    		exit(EXIT_FAILURE);
                	}

                	if (rcmd->cmd->type == EXEC){
		    		exec_cmd((struct execcmd*) rcmd->cmd);
                	}
                	else{
                    		run_cmd(rcmd->cmd);
			}
                	exit(EXIT_SUCCESS);
            	}
		if (waitpid(redrChild,&status,0) < 0){
			perror("run_cmd: redr waitpid");
			exit(EXIT_FAILURE);
	    	}
		desbloquearSenal(SIGCHLD);
            	//TRY( wait(NULL) );

	    }
            break;

        case LIST: 
            lcmd = (struct listcmd*) cmd;
            run_cmd(lcmd->left);
            run_cmd(lcmd->right);
            break;

        case PIPE: 
            pcmd = (struct pipecmd*)cmd;
            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            // Ejecución del hijo de la izquierda
	    bloquearSenal(SIGCHLD);
            if ((leftChild = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(STDOUT_FILENO) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->left->type == EXEC){
		    if (isInterno((struct execcmd*) pcmd->left))
			exec_cmdInterno((struct execcmd*) pcmd->left);
		    else
			exec_cmd((struct execcmd*) pcmd->left);
		}
                else{
                    run_cmd(pcmd->left);
		}
                exit(EXIT_SUCCESS);
            }
	    

            // Ejecución del hijo de la derecha
            if ((rightChild = fork_or_panic("fork PIPE right")) == 0)
            {
                TRY( close(STDIN_FILENO) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->right->type == EXEC){
                    if (isInterno((struct execcmd*) pcmd->right))
			exec_cmdInterno((struct execcmd*) pcmd->right);
		    else
			exec_cmd((struct execcmd*) pcmd->right);
		}
                else{
                    run_cmd(pcmd->right);
		}
                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos

            //TRY( wait(NULL) );
            //TRY( wait(NULL) );

	    if (waitpid(leftChild,&status,0) < 0){
		perror("run_cmd: pipe left waitpid");
		exit(EXIT_FAILURE);
	    }
	    if (waitpid(rightChild,&status,0) < 0){
		perror("run_cmd: pipe right waitpid");
		exit(EXIT_FAILURE);
	    }
	    desbloquearSenal(SIGCHLD);
	   
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
	    pid_t pidBack;
            if ((pidBack = fork_or_panic("fork BACK")) == 0)
            {
                if (bcmd->cmd->type == EXEC){
			if (isInterno((struct execcmd*) bcmd->cmd))
				exec_cmdInterno((struct execcmd*) bcmd->cmd);
			else
                    		exec_cmd((struct execcmd*) bcmd->cmd);
		}
		else{
                    run_cmd(bcmd->cmd);
                }
		exit(EXIT_SUCCESS);
            }
	    else{
		
		if (back_prcs < MAX_BACK){ //Se comprueba que no se supera el numero máximo de comandos en segundo plano activos
			fprintf(stdout,"[%d]\n",pidBack); //Se imprime el PID del proceso en segundo plano que se ejecuta
			int posActual = 0;
			while (backcmds[posActual] != 0){
				posActual++;
			}
			backcmds[posActual] = pidBack;
			back_prcs++;
		}else{
			printf("Se ha excedido el numero maximo de procesos en segundo plano permitidos\n");
		}
	    }
            break;

        case SUBS:
	    bloquearSenal(SIGCHLD);
            scmd = (struct subscmd*) cmd;
            if ((subsChild = fork_or_panic("fork SUBS")) == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            //TRY( wait(NULL) );
	    if (waitpid(subsChild,&status,0) < 0){
		perror("run_cmd: subs waitpid");
		exit(EXIT_FAILURE);
	    }
	    desbloquearSenal(SIGCHLD);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}


void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}



/******************************************************************************
 * Lectura de la linea de ordenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. esta permite mantener el historial, utilizar las flechas
// para acceder a las ordenes previas del historial, busquedas de ordenes, etc.

char* get_cmd()
{
    	char* buf;
    	uid_t usuario = getuid();
	struct passwd * passwd = getpwuid(usuario);
	if (!passwd){
		perror("getpwuid");
		exit(EXIT_FAILURE);
	}
	char * user = passwd->pw_name;
	char path[PATH_MAX];
	if (!getcwd(path,PATH_MAX)){
		perror("getcwd");
		exit(EXIT_FAILURE);
	}
	char * dir = basename(path);
	char prompt[strlen(user)+strlen(dir)+4];

	sprintf(prompt,"%s@<%s> ",user,dir);
    // Lee la orden tecleada por el usuario
    //buf = readline("simplesh> ");
	buf = readline(prompt);

    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf)
        add_history(buf);

    return buf;
}


/**
	Funciones de comandos internos
**/

//Funcion del comando interno exit

void run_exit(struct execcmd * ecmd){
	struct cmd * cmd = (struct cmd*) ecmd;
	free_cmd(cmd);
	free(cmd);
	exit(0); //Termina el proceso que ejecuta la llamada
}

//Funcion del comando interno cwd que muestra el path del directorio actual

void run_cwd(){
	char path[PATH_MAX];
	if (!getcwd(path,PATH_MAX)){ 
		perror("run_cwd: getcwd");
		exit(EXIT_FAILURE);
	}
	printf("cwd: %s\n",path);

}

//Funcion del comando interno cd

void run_cd(struct execcmd * ecmd){
	if (ecmd->argv[1] == NULL) { //cd sin argumentos
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		setenv("OLDPWD", path, 1);
		chdir(getenv("HOME")); //Cambia al directorio por defecto 
	}
	else if (strcmp(ecmd->argv[1],"..") == 0) { // cd ..
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		setenv("OLDPWD", path, 1);

		for (int i = strlen(path); i > 0; i--) {
			if (path[i] == '/') {
				path[i] = '\0';
				break;
			}
		}
		chdir(path); //Cambia al directorio padre del directorio actual
	}
	else if (strcmp(ecmd->argv[1],"-") == 0) { // cd -
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		char * oldpwd = getenv("OLDPWD");
		if (oldpwd == NULL) {
			printf("run_cd: Variable OLDPWD no definida\n");
		} else {
			chdir(oldpwd); //Cambia al directorio de trabajo previo
			setenv("OLDPWD", path, 1);
		}
	}
	else { //Caso en el que se especifica un directorio concreto 
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		if (ecmd->argv[2] != NULL){ //El comando cd solo puede tener un argumento  
			printf("run_cd: Demasiados argumentos\n");
		}
		else if (chdir(ecmd->argv[1]) != 0){
			//perror("run_cd");
			printf("run_cd: No existe el directorio '%s'\n",ecmd->argv[1]);
			//exit(EXIT_FAILURE);
		}else {
			setenv("OLDPWD", path, 1);
		}
	}

}

//Funcion para cerrar un descriptor de fichero
void closeFile(int fd){
	if (close(fd) < 0){
		perror("close");
                exit(EXIT_FAILURE);
	}
}

//Funcion para sincronizar un descriptor de fichero
void fsyncFile(int fd){
	if (fsync(fd) < 0){
		perror("fsync");
                exit(EXIT_FAILURE);
	}
}


void auxPsplit(int numLineas,int numBytes,int bsize,int fd,char * nombreFichero){
	int nBytesTotales = 0;
	int subfd = 0;
    	int bytesLeidos;
	int bytesEscritos;
    	int bytesRestantes = 0;
	int nLineasTotales = 0;
	int lineasLeidas;
    	char * buffer = NULL;
	int numFile = 0;
	char newFile[50];

	buffer = malloc(bsize * sizeof(char));
	if (buffer == NULL){
		perror("auxPsplit: malloc");
		exit(EXIT_FAILURE);
	}
	if (numBytes != 0){ //Caso en el que hay limite en el número de bytes
		int nBytesFA = 0;
		sprintf(newFile,"%s%d",nombreFichero,numFile);
		if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC,S_IRWXU)) < 0){
			perror("open");
			exit(EXIT_FAILURE);
		}
		while ((bytesLeidos = read(fd,buffer,bsize)) != 0){
			if (bytesLeidos < 0){
				perror("auxPsplit: read");
		         	exit(EXIT_FAILURE);
			}		
			int bytesRestantes = bytesLeidos;
			while (nBytesTotales < bytesLeidos){
				if ( nBytesFA + bytesRestantes >= numBytes ) {
					bytesEscritos = write(subfd,buffer, numBytes - nBytesFA);
					if (bytesEscritos < 0){
						perror("auxPsplit: write");
                    				exit(EXIT_FAILURE);
					}
					numFile++;
					fsyncFile(subfd);
					closeFile(subfd);
					sprintf(newFile,"%s%d",nombreFichero,numFile);
					if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC,S_IRWXU)) < 0){
						perror("auxPsplit: open");
					  	exit(EXIT_FAILURE);
					}
					nBytesTotales += bytesEscritos;
					buffer += bytesEscritos;
					nBytesFA = 0;
					bytesRestantes -= bytesEscritos;
				} else {
					bytesEscritos = write(subfd,buffer, bytesRestantes);
					if (bytesEscritos < 0){
						perror("auxPsplit: write");
                    				exit(EXIT_FAILURE);
					}
					nBytesTotales += bytesEscritos;
					buffer += bytesEscritos;
					bytesRestantes -= bytesEscritos;
					nBytesFA += bytesEscritos;
				}
			}
			buffer -= nBytesTotales;
			nBytesTotales = 0;
		
			
		}
		fsync(subfd);
		closeFile(subfd);
		closeFile(fd);
					
	}

	else if (numLineas != 0){ // Caso en el que hay limite en el numero de lineas
		int n = 0;
		sprintf(newFile,"%s%d",nombreFichero,numFile);
		if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC, S_IRWXU)) < 0){
			perror("auxPsplit: open");
                    	exit(EXIT_FAILURE);
		}
                int posicionesAvanzadas = 0;
                while ((bytesLeidos = read(fd,buffer,bsize)) != 0){
			if (bytesLeidos < 0){
				perror("auxPsplit: read");
                    		exit(EXIT_FAILURE);
			}
                	while ( nBytesTotales < bytesLeidos ) {
                        	if ( buffer[posicionesAvanzadas] == '\n' ) {
                                   	n++;
                                        if (n == numLineas) {
                                        	if (write(subfd, buffer, posicionesAvanzadas+1) < 0){
							perror("auxPsplit: write");
                    					exit(EXIT_FAILURE);
						}
                                                buffer+=posicionesAvanzadas+1;
                                                posicionesAvanzadas = 0;
						fsyncFile(subfd);
                                                closeFile(subfd);
                                                numFile++;
                                                sprintf(newFile,"%s%d",nombreFichero,numFile);
						if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC,S_IRWXU)) < 0){
							perror("auxPsplit: open");
                    					exit(EXIT_FAILURE);
						}
                                                n = 0;
                                         }
	   				 else
						posicionesAvanzadas++;					
				} else
					posicionesAvanzadas++;
                                nBytesTotales++;
				
                        }
			if (posicionesAvanzadas > 0){
				if (write(subfd, buffer, posicionesAvanzadas) < 0){
					perror("auxPsplit: write");
                    			exit(EXIT_FAILURE);
				}
			}
			buffer = buffer - nBytesTotales + posicionesAvanzadas;
			posicionesAvanzadas = 0;
			nBytesTotales = 0;
        	}
		fsyncFile(subfd);
                closeFile(subfd);
		closeFile(fd);
		
	}
	
	else if (numBytes == 0 && numLineas == 0){ //Caso en el que no se especifica ni el numero maximo de bytes por fichero ni el numero maximo de lineas 
		sprintf(newFile,"%s%d",nombreFichero,numFile);
		if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC, S_IRWXU)) < 0){
			perror("auxPsplit: open");
                   	exit(EXIT_FAILURE);
		}
		while ((bytesLeidos = read(fd,buffer,bsize)) != 0){ 
			if (write(subfd,buffer,bytesLeidos) < 0){
				perror("auxPsplit: write");
                    		exit(EXIT_FAILURE);
			}
			fsyncFile(subfd);
			closeFile(subfd);
			numFile++;
			sprintf(newFile,"%s%d",nombreFichero,numFile);
			if ((subfd = open(newFile,O_CREAT | O_RDWR | O_TRUNC, S_IRWXU)) < 0){
				perror("auxPsplit: open");
                    		exit(EXIT_FAILURE);
			}
		}
		closeFile(fd);
	}
	free(buffer); 
	//printf("%s\n", ecmd->argv[i]);
}

//Funcion del comando interno psplit 


void run_psplit(struct execcmd * ecmd){
    optind = 1;
    char opt;
    pid_t frk;
    int fd;
    int opcionAyuda = 0;
    int status;
    int numLineas = 0;
    int numBytes = 0;
    int bsize = 1024; 
    int procs = 1; 
    bloquearSenal(SIGCHLD); //Se bloquea la señal SIGCHLD para que no se ejecute el waitpid del manejador
    while ((opt = getopt(ecmd->argc, ecmd->argv, "l:b:s:p:h")) != -1) { //Parametro con : quiere decir que va seguido de un valor
        switch (opt) {
            case 'l':  
                numLineas = atoi(optarg);
                break;
            case 'b': 
                numBytes = atoi(optarg); 
                break;
	    case 's':  
		bsize = atoi(optarg);
		break;
	    case 'p': 
		procs = atoi(optarg);
		break;
	    case 'h': 
	    default:
		opcionAyuda = 1;
		printf("Uso: %s [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n", ecmd->argv[0]);
		printf("\tOpciones:\n");
		printf("	-l NLINES Número máximo de líneas por fichero.\n");
		printf("	-b NBYTES Número maximo de bytes por fichero\n");
		printf("	-s BSIZE  Tamaño en bytes de los bloques leidos de [FILEn] o stdin\n");
		printf("	-p PROCS  Número máximo de procesos simultáneos\n");
		printf("	-h        Ayuda\n\n");

		break;
            
        }
    }

    if (numLineas != 0 && numBytes!= 0){
	printf("psplit: Opciones incompatibles\n");
    }
    else if (bsize < MIN_BSIZE || bsize > MAX_BSIZE){
	printf("psplit: Opción -s no válida\n");
    }
    else if (procs == 0){
	printf("psplit: Opción -p no válida\n");
    }
    else if (opcionAyuda){}

    else if (optind == ecmd->argc){  //No hay ficheros de entrada
	frk = fork_or_panic("fork psplit");
	if (frk == 0){
		auxPsplit(numLineas,numBytes,bsize,STDIN_FILENO,"stdin");
		exit(EXIT_SUCCESS);
	}
	if (waitpid(frk,&status,0) < 0){
		perror("run_psplit: waitpid");
		exit(EXIT_FAILURE);
	}
    }

	else { //Procesamiento de ficheros de entrada
		int i = optind;
		pid_t * pids = malloc(procs * sizeof(pid_t));
		memset(pids,0,sizeof(pid_t));
		if (pids == NULL){
			perror("run_psplit: malloc");
			exit(EXIT_FAILURE);
		}
		int nprocs = 0;
		int indexAdd = 0;
		int indexWait = 0;
		for(int i = optind; i < ecmd->argc; i++){
			frk = fork_or_panic("fork psplit");
			if (frk == 0) {
				if ((fd = open(ecmd->argv[i],O_RDONLY,S_IRWXU)) < 0){
					perror("run_psplit: open");
                    			exit(EXIT_FAILURE);
				} 
				auxPsplit(numLineas,numBytes,bsize,fd,ecmd->argv[i]);
				exit(EXIT_SUCCESS);
			} 
			pids[indexAdd] = frk;
			nprocs++;
			if (indexAdd + 1 == procs)
				indexAdd = 0;
			else 
				indexAdd++;
			
			if ( nprocs == procs ) {
					if(waitpid((pid_t)pids[indexWait],&status,0) < 0){
							perror("run_psplit: waitpid");
							exit(EXIT_FAILURE);
						}
					if (indexWait + 1 == procs)
						indexWait = 0;
					else 
						indexWait++;
					nprocs--;
			} 
			if (ecmd->argc - i == 1 ) {
				while(nprocs > 0) {
					if(waitpid((pid_t)pids[indexWait],&status,0) < 0){
							perror("run_psplit: waitpid");
							exit(EXIT_FAILURE);
						}
					if (indexWait + 1 == procs)
						indexWait = 0;
					else 
						indexWait++;
					nprocs--;

				} 
			}

		}
		free(pids);
		desbloquearSenal(SIGCHLD); //Tras haberse ejecutado los waitpid anteriores se desbloquea la señal SIGCHLD
    	}    
		 
}

//Funcion del comando interno bjobs

void run_bjobs(struct execcmd * ecmd){
	char opt;
	optind = 1;
	//opterr = 0;
	int opcionKill = 0;
	//if(ecmd->argc > 1)
		while ((opt = getopt(ecmd->argc, ecmd->argv, "kh")) != -1) { //Se procesa la opción indicada
			switch (opt) {
		    		case 'k':
		        		opcionKill = 1;
		        		break;
		    		case 'h': //Opcion de ayuda
				default:
					//opcionAyuda = 1;
					printf("Uso: %s [-k] [-h]\n", ecmd->argv[0]);
					printf("\tOpciones:\n");
					printf("\t-k Mata todos los procesos en segundo plano.\n");
					printf("\t-h Ayuda\n\n");
					break;
				
				}
		}

	if (opcionKill){ //Caso en el que hay que matar a todos los procesos en segundo plano activos
		for (int i=0; i< MAX_BACK;i++){
			if (backcmds[i] != 0){
				if (kill(backcmds[i], SIGTERM) == -1){ //Se envia la señal SIGTERM a cada proceso 
					perror("run_bjobs: kill");
					exit(EXIT_FAILURE);
				}
			}
		}	
			
	}
	else{ //Caso en el que se muestra los PID de los procesos en segundo plano activos
		for (int i=0; i< MAX_BACK;i++){
			if (backcmds[i] != 0){
				fprintf(stdout,"[%d]\n",backcmds[i]);	
			}
			
		}
	} 
       
}


//Funcion para ejecutar comandos internos

void exec_cmdInterno(struct execcmd * ecmd){
	assert(ecmd->type == EXEC);
	if (strcmp(ecmd->argv[0],"cwd") == 0){ //Caso en el que se ejecuta el comando cwd
		run_cwd();
	}
	else if (strcmp(ecmd->argv[0],"exit") == 0){ //Caso en el que se ejecuta el comando exit
		run_exit(ecmd);
	}
	else if (strcmp(ecmd->argv[0], "cd") == 0) { //Caso en el que se ejecuta el comando cd
		run_cd(ecmd);
	}
	else if (strcmp(ecmd->argv[0], "psplit") == 0) { //Caso en el que se ejecuta el comando psplit 
		run_psplit(ecmd);
	}

	else if (strcmp(ecmd->argv[0], "bjobs") == 0){ //Caso en el que se ejecuta el comando bjobs
		run_bjobs(ecmd);
	}

}

//Funcion que comprueba si un comando es interno

int isInterno(struct execcmd* ecmd){

	if (ecmd->argv[0] == NULL) return 0;
	for (int i=0;i<NUM_INTERNOS;i++){
		if (strcmp(ecmd->argv[0],cmdInternos[i]) == 0){
			return 1;
		}
	}
	return 0;
}


/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parametros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }

}


//Manejador de señales
void handle_sigchld(int sig) {
  int saved_errno = errno;
  pid_t pidChild;
  if (sig == SIGCHLD){ //Tratamiento para la señal SIGCHLD
	while ((pidChild = waitpid((pid_t)-1,0,WNOHANG)) > 0) {
		//printf("Entra señal SIGCHLD\n");
		for (int i=0; i< MAX_BACK;i++){
			if (backcmds[i] == pidChild){
				fprintf(stdout,"[%d]\n",pidChild); //Se muestra el PID del proceso que ha terminado
				backcmds[i] = 0;
				back_prcs--;
				//printf("Número de procesos activos%d\n",back_prcs);
			}
		}
  	}

  }
  else if (sig == SIGTERM){ //Tratamiento para la señal SIGTERM
	//printf("Entra señal SIGTERM\n");
	exit(EXIT_SUCCESS);
  }
  

  errno = saved_errno;
}

int main(int argc, char** argv)
{
    char* buf;
    struct cmd* cmd;
    struct sigaction sa1; //Estructura sigaction para la señal SIGQUIT
    struct sigaction sa;  //Estructura sigaction para la señal SIGCHLD y SIGTERM
    //Inicializacion de sa1
    memset(&sa1, 0, sizeof(sa1)); 
    sa1.sa_handler = SIG_IGN; //Se establece la accion de ignorar la señal
    //sigemptyset(&sa.sa_mask);
    
    //Inicializacion de sa
    memset(&sa, 0, sizeof(sa)); 
    sa.sa_handler = handle_sigchld; //Se establece el manejador de señales
    if (sigemptyset(&sa.sa_mask) < 0){
	perror("main: sigemptyset1");
        exit(EXIT_FAILURE);
    }
    sa.sa_flags = 0;

    //sigset_t blocked_signals;
    if (sigemptyset(&blocked_signals) < 0){
	perror("main: sigemptyset2");
        exit(EXIT_FAILURE);
    }
    if (sigaddset(&blocked_signals, SIGINT) < 0){
	perror("main: sigaddset");
        exit(EXIT_FAILURE);
    }
    //Se añade la señal SIGINT a la mascara de bloqueo de señales
    if (sigprocmask(SIG_BLOCK, &blocked_signals, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
    //Se establece la accion a realizar al recibir una señal SIGQUIT
    // que en este caso es ignorada
    if (sigaction(SIGQUIT,&sa1, NULL) == -1) {
        perror("main: sigaction1");
        exit(EXIT_FAILURE);
    }

    //Se establece la acción a realizar al recibir una señal SIGCHLD que en este caso
    // esta definida por el handler
    if (sigaction(SIGCHLD,&sa, NULL) == -1) {
        perror("main: sigaction2");
        exit(EXIT_FAILURE);
    }
    //Se establece la acción a realizar al recibir una señal SIGTERM que en este caso
    // esta definida por el handler
    if (sigaction(SIGTERM,&sa, NULL) == -1) {
        perror("main: sigaction3");
        exit(EXIT_FAILURE);
    }

    parse_args(argc, argv);
    unsetenv("OLDPWD");

    DPRINTF(DBG_TRACE, "STR\n");

    // Bucle de lectura y ejecucion de ordenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el analisis sintactico de la linea de ordenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la linea de ordenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);
	free(cmd);
        // Libera la memoria de la linea de ordenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;

}
