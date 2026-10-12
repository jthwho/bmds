/*
 * Dropbear's options for BMDS (devplan/network.md §24); the rest are default_options.h's.
 */

/* No fork: the server's connections and sessions are vfork + exec (patch 0001; with HAVE_FORK
 * undefined, its uClinux paths start shells and commands with vfork). */
#define DROPBEAR_VFORK_REEXEC 1


/* PATH comes from the shell (shell/path), not a fixed list (patch 0002). */
#define DROPBEAR_NO_DEFAULT_PATH 1

/* The kernel starts the server with -F -E; no pid file. */
#define DROPBEAR_PIDFILE ""

/* Our sftp-server, found on PATH. */
#define DROPBEAR_SFTPSERVER 1
#define SFTPSERVER_PATH "sftp-server"

/* No X11; agent forwarding needs AF_UNIX, which comes later. */
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0

/* Host keys: Ed25519 (made by the kernel) and ECDSA; RSA kept for the client's peers; no DSS. */
#define DROPBEAR_DSS 0

/* No message of the day. */
#define DO_MOTD 0

/* scp runs the client by name, found on PATH (execvp), not a fixed path. */
#define DROPBEAR_PATH_SSH_PROGRAM "dbclient"

/* libtomcrypt's AES-CTR and SHAs on the chip's engines through the kernel's crypto call, software
 * on a kernel without it (patch 0004, devplan/network.md §26). */
#define BMDS_CRYPTO_ENGINES 1
