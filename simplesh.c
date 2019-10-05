/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * AmpliaciÃ³n de Sistemas Operativos
 * Departamento de IngenierÃ­a y TecnologÃ­a de Computadores
 * Facultad de InformÃ¡tica de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (vÃ©ase /usr/include/features.h) */
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



// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.19";

// Niveles de depuraciÃ³n
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


// NÃºmero mÃ¡ximo de argumentos de un comando
#define MAX_ARGS 16
//NÃºmero de comandos internos
#define NUM_INTERNOS 4

//Array de comandos internos
const char * cmdInternos[NUM_INTERNOS] = {"cwd","cd","exit","psplit"};

// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";

char pathAnterior[PATH_MAX];
int std_out;

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


// Imprime el mensaje de error y aborta la ejecuciÃ³n
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


// Las estructuras `cmd` se utilizan para almacenar informaciÃ³n que servirÃ¡ a
// simplesh para ejecutar lÃ­neas de Ã³rdenes con redirecciones, tuberÃ­as, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// NÃ³tese cÃ³mo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de Ã©l se obtiene un tipo derivado a travÃ©s de
// *casting* forzado de tipo. Se consigue asÃ­ polimorfismo bÃ¡sico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parÃ¡metros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirecciÃ³n
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tuberÃ­a
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de Ã³rdenes
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
 * Funciones para realizar el anÃ¡lisis sintÃ¡ctico de la lÃ­nea de Ã³rdenes
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
            // *despuÃ©s* del argumento. Por ejemplo:
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
// El primer puntero pasado como parÃ¡mero (`start_of_str`) avanza hasta el
// primer carÃ¡cter que no estÃ¡ en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo despuÃ©s de los caracteres en `WHITESPACE`.

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


// `parse_cmd` realiza el *anÃ¡lisis sintÃ¡ctico* de la lÃ­nea de Ã³rdenes
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

    // Comprueba que se ha alcanzado el final de la lÃ­nea de Ã³rdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintÃ¡ctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el anÃ¡lisis sintÃ¡ctico de la lÃ­nea de Ã³rdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la lÃ­nea contiene alguna tuberÃ­a.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de Ã³rdenes y/o redirecciones.  A continuaciÃ³n, `parse_line`
// comprueba si la ejecuciÃ³n de la lÃ­nea se realiza en segundo plano (con `&`)
// o si la lÃ­nea de Ã³rdenes contiene una lista de Ã³rdenes (con `;`).

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
            error("%s: error sintÃ¡ctico: no se encontrÃ³ comando\n", __func__);

        // Consume el delimitador de lista de Ã³rdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el anÃ¡lisis sintÃ¡ctico de una tuberÃ­a de manera
// recursiva si encuentra el delimitador de tuberÃ­as '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el anÃ¡lisis sintÃ¡ctico de todos los componentes de la tuberÃ­a.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintÃ¡ctico: no se encontrÃ³ comando\n", __func__);

        // Consume el delimitador de tuberÃ­a
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tuberÃ­a
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el anÃ¡lisis sintÃ¡ctico de un comando a no ser que la
// expresiÃ³n comience por un parÃ©ntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y despuÃ©s del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // Â¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una lÃ­nea de Ã³rdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // Â¿Redirecciones antes del comando?
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
            error("%s: error sintÃ¡ctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // Â¿Redirecciones despuÃ©s del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene mÃ¡s parÃ¡metros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el anÃ¡lisis sintÃ¡ctico de un bloque de Ã³rdenes
// delimitadas por parÃ©ntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones despuÃ©s del bloque de Ã³rdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el parÃ©ntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintÃ¡ctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el anÃ¡lisis sintÃ¡ctico hasta el parÃ©ntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de Ã³rdenes
    cmd = subscmd(scmd);

    // Consume el parÃ©ntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintÃ¡ctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // Â¿Redirecciones despuÃ©s del bloque de Ã³rdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el anÃ¡lisis sintÃ¡ctico de Ã³rdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirecciÃ³n ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuaciÃ³n es delimitador de
    // redirecciÃ³n...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirecciÃ³n
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirecciÃ³n entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintÃ¡ctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirecciÃ³n
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

/******************************************************************************
 * Funciones para la ejecuciÃ³n de la lÃ­nea de Ã³rdenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontrÃ³ el comando '%s'\n", ecmd->argv[0]);
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

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            	ecmd = (struct execcmd*) cmd;
	    	if (isInterno(ecmd))
			exec_cmdInterno(ecmd);
		else{
        	    	if (fork_or_panic("fork EXEC") == 0){
				exec_cmd(ecmd);
			}
			TRY( wait(NULL) );
		}

            	break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
 	    if (isInterno((struct execcmd*) rcmd->cmd)){
		if ((std_out = dup(1)) == -1){
		    perror("dup");
                    exit(EXIT_FAILURE);
		}
		TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags,rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }	
	    	if (rcmd->cmd->type == EXEC)
			exec_cmdInterno((struct execcmd*) rcmd->cmd);
		else
			run_cmd(rcmd->cmd);
		//Se cierra el fichero creado para la redireccion
		if (close(fd) == -1){
		    perror("close");
                    exit(EXIT_FAILURE);
		}
		//Se vuelve a abrir la salida estandar
		if (dup2(std_out,1) == -1){
			perror("dup2");
			exit(EXIT_FAILURE);
		}
	    } else {
		if (fork_or_panic("fork REDR") == 0){
                	TRY( close(rcmd->fd) );
                	if ((fd = open(rcmd->file, rcmd->flags,rcmd->mode)) < 0){
                    		perror("open");
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
            	TRY( wait(NULL) );
		
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

            // EjecuciÃ³n del hijo de la izquierda
            if (fork_or_panic("fork PIPE left") == 0)
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

            // EjecuciÃ³n del hijo de la derecha
            if (fork_or_panic("fork PIPE right") == 0)
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
            TRY( wait(NULL) );
            TRY( wait(NULL) );
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            if (fork_or_panic("fork BACK") == 0)
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
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            if (fork_or_panic("fork SUBS") == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( wait(NULL) );
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
 * Lectura de la lÃ­nea de Ã³rdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ãsta permite mantener el historial, utilizar las flechas
// para acceder a las Ã³rdenes previas del historial, bÃºsquedas de Ã³rdenes, etc.

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
	exit(0);
}

//Funcion del comando interno cwd

void run_cwd(){
	char path[PATH_MAX];
	if (!getcwd(path,PATH_MAX)){
		perror("run_cwd");
		exit(EXIT_FAILURE);
	}
	printf("cwd: %s\n",path);

}

//Funcion del comando interno cd

void run_cd(struct execcmd * ecmd){
	if (ecmd->argv[1] == NULL) {
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		setenv("OLDPWD", path, 1);
		chdir(getenv("HOME"));
	}
	else if (strcmp(ecmd->argv[1],"..") == 0) {
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		setenv("OLDPWD", path, 1);

		for (int i = strlen(path); i > 0; i--) {
			if (path[i] == '/') {
				path[i] = '\0';
				break;
			}
		}
		chdir(path);
	}
	else if (strcmp(ecmd->argv[1],"-") == 0) {
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		char * oldpwd = getenv("OLDPWD");
		if (oldpwd == NULL) {
			printf("run_cd: Variable OLDPWD no definida\n");
		} else {
			chdir(oldpwd);
			setenv("OLDPWD", path, 1);
		}
	}
	else {
		char path[PATH_MAX];
		getcwd(path,PATH_MAX);
		if (ecmd->argv[2] != NULL){
			printf("run_cd: Demasiados argumentos\r\n");
		}
		else if (chdir(ecmd->argv[1]) != 0){
			//perror("run_cd");
			printf("run_cd: No existe el directorio '%s'\r\n",ecmd->argv[1]);
			//exit(EXIT_FAILURE);
		}else {
			setenv("OLDPWD", path, 1);
		}
	}

}

//Funcion del comando interno psplit 

void run_psplit(struct execcmd * ecmd){
    optind = 1;
    char opt;
    int numLineas = 0;
    int numBytes = 0;
    int nBytesTotales = 0;
    int nLineasTotales = 0;
    int bsize = 1024;
    int subfd = 0;
    int bytesLeidos;
    int lineasLeidas;
    char * buffer = NULL;
    
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

		break;
	    case 'h':
		printf("Uso: %s [-l NUM] [-b NUM] [-s NUM] [-p NUM] [-h] [FILE1] [FILE2]...\n", ecmd->argv[0]);
		printf("\tOpciones:\n");
		printf("\t-l NLINES Número máximo de lineas por fichero.\n");
		printf("\t-b NBYTES Número máximo de bytes por fichero\n");
		printf("\t-s BSIZE  Tamaño en bytes de los bloques leidos de [FILEn] o stdin\n");
		printf("\t-p PROCS  Número máximo de procesos simultáneos\n");
		printf("\t-h        Ayuda\n");
		break;
            default:
                fprintf(stderr, "Usage: %s [-l] [-l NUM] [-b NUM] [-s NUM] [-p NUM] [-h]\n", ecmd->argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (optind == ecmd->argc){  //No hay ficheros de entrada
	/*if (numLineas != 0 && numBytes!= 0){
		printf("psplit: Opciones incompatibles\n");
	}
	if (numLineas != 0){

	}
	else if (numBytes != 0){
		
	}*/
    }
    //Procesamiento de ficheros de entrada
    for(int i = optind; i < ecmd->argc; i++){
	if (numLineas != 0 && numBytes!= 0){
		printf("psplit: Opciones incompatibles\n");
	}
	else {
		buffer = malloc(bsize * sizeof(char));
		if (buffer == NULL){
			printf("Fallo al reservar memoria con malloc\n");
			exit(EXIT_FAILURE);
		}
		int numFile = 0;
		char newFile[50];
		int fd = open(ecmd->argv[i],O_RDONLY,S_IRWXU);
		if (numBytes != 0){ //Caso en el que hay limite en el numero de bytes
			while ((bytesLeidos = read(fd,buffer,bsize)) != 0){		
				sprintf(newFile,"%s%d",ecmd->argv[i],numFile);
				while (nBytesTotales < bsize){
					sprintf(newFile,"%s%d",ecmd->argv[i],numFile);
					subfd = open(newFile,O_CREAT | O_RDWR | O_APPEND,S_IRWXU);
					write(subfd, buffer, numBytes);
					numFile++;
					buffer += numBytes;
					nBytesTotales += numBytes;
					
				}
				buffer -= nBytesTotales;
				nBytesTotales = 0;
			}
					
		}
		else if (numLineas != 0){ // Caso en el que hay limite en el numero de lineas
			sprintf(newFile,"%s%d",ecmd->argv[i],numFile);
			subfd = open(newFile,O_CREAT | O_RDWR | O_APPEND,S_IRWXU);	
			while ((bytesLeidos = read(fd,buffer,bsize)) != 0) {	
				while (nBytesTotales < bsize) {
					while (buffer[0]!='\n' && buffer[0]!='\0') {
						write(subfd, buffer, 1);
						buffer++;
						nBytesTotales++;
					}
					if (buffer[0] == '\n') {
						write(subfd, buffer, 1);
						buffer++;
						nBytesTotales++;
						nLineasTotales++;
						if (nLineasTotales == numLineas) {
							numFile++;
							sprintf(newFile,"%s%d",ecmd->argv[i],numFile);
							subfd = open(newFile,O_CREAT | O_RDWR | O_APPEND,S_IRWXU);	
							nLineasTotales = 0;
						}
					} else 
						break;
				}
				buffer-=nBytesTotales;
				nBytesTotales = 0;
			}

		
		}

		else if (numBytes == 0 && numLineas == 0){
			while ((bytesLeidos = read(fd,buffer,bsize)) != 0){ 
				sprintf(newFile,"%s%d",ecmd->argv[i],numFile);
				subfd = open(newFile,O_CREAT | O_RDWR | O_APPEND,S_IRWXU);
				write(subfd,buffer,bsize);
				close(subfd);
				numFile++;
			}
		}
				
	}
	free(buffer);
	//printf("%s\n", ecmd->argv[i]);
    }	 
    
}


//Funcion para ejecutar comandos internos

void exec_cmdInterno(struct execcmd * ecmd){
	assert(ecmd->type == EXEC);
	if (strcmp(ecmd->argv[0],"cwd") == 0){
		run_cwd();
	}
	else if (strcmp(ecmd->argv[0],"exit") == 0){
		run_exit(ecmd);
	}
	else if (strcmp(ecmd->argv[0], "cd") == 0) {
		run_cd(ecmd);
	}
	else if (strcmp(ecmd->argv[0], "psplit") == 0) {
		run_psplit(ecmd);
	}
}

//Funcion que comprueba si un comando es interno

int isInterno(struct execcmd* ecmd){

	if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);
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

    // Bucle de procesamiento de parÃ¡metros
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


int main(int argc, char** argv)
{
    char* buf;
    struct cmd* cmd;

    parse_args(argc, argv);
    unsetenv("OLDPWD");

    DPRINTF(DBG_TRACE, "STR\n");

    // Bucle de lectura y ejecuciÃ³n de Ã³rdenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el anÃ¡lisis sintÃ¡ctico de la lÃ­nea de Ã³rdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la lÃ­nea de Ã³rdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);
	free(cmd);
        // Libera la memoria de la lÃ­nea de Ã³rdenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;

}

