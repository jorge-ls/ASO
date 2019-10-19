/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * AmpliaciÃÂÃÂ³n de Sistemas Operativos
 * Departamento de IngenierÃÂÃÂ­a y TecnologÃÂÃÂ­a de Computadores
 * Facultad de InformÃÂÃÂ¡tica de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (vÃÂÃÂ©ase /usr/include/features.h) */
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

// Niveles de depuraciÃÂÃÂ³n
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


// Número máximo de argumentos de un comando
#define MAX_ARGS 16
//Número de comandos internos
#define NUM_INTERNOS 5
//Tamaño minimo del buffer de lectura
#define MIN_BSIZE 1
//Tamaño maximo del bloque de lectura
#define MAX_BSIZE pow(2,20)
//Número máximo de comandos activos en segundo plano
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


// Imprime el mensaje de error y aborta la ejecuciÃÂÃÂ³n
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


// Las estructuras `cmd` se utilizan para almacenar informaciÃÂÃÂ³n que servirÃÂÃÂ¡ a
// simplesh para ejecutar lÃÂÃÂ­neas de ÃÂÃÂ³rdenes con redirecciones, tuberÃÂÃÂ­as, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// NÃÂÃÂ³tese cÃÂÃÂ³mo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de ÃÂÃÂ©l se obtiene un tipo derivado a travÃÂÃÂ©s de
// *casting* forzado de tipo. Se consigue asÃÂÃÂ­ polimorfismo bÃÂÃÂ¡sico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parÃÂÃÂ¡metros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirecciÃÂÃÂ³n
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tuberÃÂÃÂ­a
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de ÃÂÃÂ³rdenes
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
 * Funciones para realizar el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes
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
            // *despuÃÂÃÂ©s* del argumento. Por ejemplo:
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
// El primer puntero pasado como parÃÂÃÂ¡mero (`start_of_str`) avanza hasta el
// primer carÃÂÃÂ¡cter que no estÃÂÃÂ¡ en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo despuÃÂÃÂ©s de los caracteres en `WHITESPACE`.

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


// `parse_cmd` realiza el *anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico* de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes
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

    // Comprueba que se ha alcanzado el final de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintÃÂÃÂ¡ctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la lÃÂÃÂ­nea contiene alguna tuberÃÂÃÂ­a.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de ÃÂÃÂ³rdenes y/o redirecciones.  A continuaciÃÂÃÂ³n, `parse_line`
// comprueba si la ejecuciÃÂÃÂ³n de la lÃÂÃÂ­nea se realiza en segundo plano (con `&`)
// o si la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes contiene una lista de ÃÂÃÂ³rdenes (con `;`).

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
            error("%s: error sintÃÂÃÂ¡ctico: no se encontrÃÂÃÂ³ comando\n", __func__);

        // Consume el delimitador de lista de ÃÂÃÂ³rdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de una tuberÃÂÃÂ­a de manera
// recursiva si encuentra el delimitador de tuberÃÂÃÂ­as '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de todos los componentes de la tuberÃÂÃÂ­a.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintÃÂÃÂ¡ctico: no se encontrÃÂÃÂ³ comando\n", __func__);

        // Consume el delimitador de tuberÃÂÃÂ­a
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tuberÃÂÃÂ­a
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de un comando a no ser que la
// expresiÃÂÃÂ³n comience por un parÃÂÃÂ©ntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y despuÃÂÃÂ©s del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ÃÂÃÂ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una lÃÂÃÂ­nea de ÃÂÃÂ³rdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ÃÂÃÂ¿Redirecciones antes del comando?
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
            error("%s: error sintÃÂÃÂ¡ctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ÃÂÃÂ¿Redirecciones despuÃÂÃÂ©s del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene mÃÂÃÂ¡s parÃÂÃÂ¡metros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de un bloque de ÃÂÃÂ³rdenes
// delimitadas por parÃÂÃÂ©ntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones despuÃÂÃÂ©s del bloque de ÃÂÃÂ³rdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el parÃÂÃÂ©ntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintÃÂÃÂ¡ctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico hasta el parÃÂÃÂ©ntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de ÃÂÃÂ³rdenes
    cmd = subscmd(scmd);

    // Consume el parÃÂÃÂ©ntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintÃÂÃÂ¡ctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ÃÂÃÂ¿Redirecciones despuÃÂÃÂ©s del bloque de ÃÂÃÂ³rdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el anÃÂÃÂ¡lisis sintÃÂÃÂ¡ctico de ÃÂÃÂ³rdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirecciÃÂÃÂ³n ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuaciÃÂÃÂ³n es delimitador de
    // redirecciÃÂÃÂ³n...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirecciÃÂÃÂ³n
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirecciÃÂÃÂ³n entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintÃÂÃÂ¡ctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirecciÃÂÃÂ³n
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
 * Funciones para la ejecuciÃÂÃÂ³n de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontrÃÂÃÂ³ el comando '%s'\n", ecmd->argv[0]);
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

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            	ecmd = (struct execcmd*) cmd;
	    	if (isInterno(ecmd))
			exec_cmdInterno(ecmd);
		else{
        	    	if ((pidChild = fork_or_panic("fork EXEC")) == 0){
				exec_cmd(ecmd);
			}
			//TRY( wait(NULL) );
			if (waitpid(pidChild,&status,0) < 0){
				perror("run_cmd: waitpid");
				exit(EXIT_FAILURE);
			}
		}

            	break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
 	    if (isInterno((struct execcmd*) rcmd->cmd)){
		if ((std_out = dup(1)) == -1){
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
		if (fork_or_panic("fork REDR") == 0){
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

            // Ejecución del hijo de la izquierda
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
		perror("run_cmd: waitpid");
		exit(EXIT_FAILURE);
	    }
	    if (waitpid(rightChild,&status,0) < 0){
		perror("run_cmd: waitpid");
		exit(EXIT_FAILURE);
	    }
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
		fprintf(stdout,"[%d]\n",pidBack);
		if (back_prcs < MAX_BACK){
			backcmds[back_prcs] = pidBack;
			back_prcs++;
		}
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
 * Lectura de la lÃÂÃÂ­nea de ÃÂÃÂ³rdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. ÃÂÃÂsta permite mantener el historial, utilizar las flechas
// para acceder a las ÃÂÃÂ³rdenes previas del historial, bÃÂÃÂºsquedas de ÃÂÃÂ³rdenes, etc.

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
		perror("run_cwd: getcwd");
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


void closeFile(int fd){
	if (close(fd) < 0){
		perror("close");
                exit(EXIT_FAILURE);
	}
}

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
	
	else if (numBytes == 0 && numLineas == 0){
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
		opcionAyuda = 1;
		printf("Uso: %s [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n", ecmd->argv[0]);
		printf("\tOpciones:\n");
		printf("\t-l NLINES Número máximo de líneas por fichero.\n");
		printf("\t-b NBYTES Número máximo de bytes por fichero\n");
		printf("\t-s BSIZE  Tamaño en bytes de los bloques leidos de [FILEn] o stdin\n");
		printf("\t-p PROCS  Número máximo de procesos simultaneos\n");
		printf("\t-h	Ayuda\n\n");
		break;
            default:
         	
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
		if (pids == NULL){
			perror("run_psplit: malloc");
			exit(EXIT_FAILURE);
		}
		int nprocs = 0;
		for(int i = optind; i < ecmd->argc; i++){
			frk = fork_or_panic("fork psplit");
			if ( frk == 0 ) {
				if ((fd = open(ecmd->argv[i],O_RDONLY,S_IRWXU)) < 0){
					perror("run_psplit: open");
                    			exit(EXIT_FAILURE);
				}
				auxPsplit(numLineas,numBytes,bsize,fd,ecmd->argv[i]);
				exit(EXIT_SUCCESS);
			}
			pids[nprocs] = frk;
			nprocs++;
			//Esperamos a los procesos anteriores cuando cuando se alcanza el numero máximo de procesos en vuelo permitido
			// o cuando se llega al ultimo fichero
			if ( nprocs == procs || ecmd->argc - i == 1 ) { 
				for (int i=0;i<nprocs;i++){
					if(waitpid(pids[i],&status,0) < 0){
						perror("run_psplit: waitpid");
						exit(EXIT_FAILURE);
					}
				}
				nprocs = 0;
			}		
			

		}
		free(pids);
    	}    
	
	/*else{
	//Procesamiento de ficheros de entrada

	for(int i = optind; i < ecmd->argc; i++){
		//if (fork_or_panic("fork psplit") == 0){
			int fd = open(ecmd->argv[i],O_RDONLY,S_IRWXU);
			auxPsplit(numLineas,numBytes,bsize,fd,ecmd->argv[i]);
		//}
	}
    
	
	*/	 
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

    // Bucle de procesamiento de parámetros
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

//Manejador de señales para la señal SIGCHILD

void handle_sigchld(int sig) {
  int saved_errno = errno;
  pid_t pidChild;
  while ((pidChild = waitpid((pid_t)-1,0,WNOHANG)) > 0) {
	for (int i=0; i< MAX_BACK;i++){
		if (backcmds[i] == pidChild){
			fprintf(stdout,"[%d]\n",pidChild);
		}
	}
  }
  /*if (pidChild == -1){
	perror("handle_sigchld: waitpid");
        exit(EXIT_FAILURE);
  }*/
  errno = saved_errno;
}

int main(int argc, char** argv)
{
    char* buf;
    struct cmd* cmd;
    struct sigaction sa;  //Estructura sigaction para la señal SIGCHLD
    struct sigaction sa1; //Estructura sigaction para la señal SIGQUIT	
    //Inicializacion de sa1
    memset(&sa1, 0, sizeof(sa1)); 
    sa1.sa_handler = SIG_IGN;
    //sigemptyset(&sa.sa_mask);
    
    //Inicializacion de sa
    memset(&sa, 0, sizeof(sa)); 
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigset_t blocked_signals;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGINT);
    //Se añade la señal SIGINT a la mascara de bloqueo de señales
    /*if (sigprocmask(SIG_BLOCK, &blocked_signals, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }*/
    //Se establece la accion a realizar al recibir una señal SIGQUIT
    // que en este caso es ignorada
    if (sigaction(SIGQUIT,&sa1, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    //Se establece la acción a realiar al recibir una señal SIGCHLD que en este caso
    // esta definida por el handler
    if (sigaction(SIGCHLD,&sa, NULL) == -1) {
        perror("sigaction");
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
