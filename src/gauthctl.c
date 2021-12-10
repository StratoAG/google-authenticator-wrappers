/* gauthctl -- manage secure gauth configs
 * (c) 2018 Michał Górny
 *     2021 extended Björn Adler, STRATO AG
 * Licensed under 2-clause BSD license
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_misc.h>

/* constant used for assert(not_reached) */
static const bool not_reached = false;

/* program long options */
const struct option long_opts[] = {
	{"enable", no_argument, NULL, 'e'},
	{"disable", required_argument, NULL, 'd'},
	{"status", no_argument, NULL, 's'},
	{"help", no_argument, NULL, 'h'},
	{"version", no_argument, NULL, 'V'},
	{NULL},
};

/* enum to keep selected command */
enum command
{
	CMD_NULL,
	CMD_ENABLE,
	CMD_DISABLE,
    CMD_STATUS,
	CMD_NEXT
};

/**
 * print usage or full help message
 * prog_name: program name to print (from argv[0])
 * help: true for full help message, false for short usage
 * returns exit status for program (0 for help, 1 otherwise)
 */
int usage(const char* prog_name, bool help)
{
	FILE* const out = help ? stdout : stderr;

	fprintf(out, "Usage: %s --enable\n", prog_name);
	if (help)
		fputs("            Enable gauth using config supplied on fd 3\n", out);
	fprintf(out, "       %s --disable username\n", prog_name);
	if (help)
		fputs("            Disable gauth for given user\n", out);
	fprintf(out, "       %s --status\n", prog_name);
	if (help)
		fputs("            Checks status of gauth for the user\n", out);
	return help ? 0 : 1;
}

/**
 * get the username for spawning user
 * returns pointer to const username string
 */
const char* get_user()
{
	const struct passwd* pw = getpwuid(getuid());

	if (!pw)
		return NULL;
	return pw->pw_name;
}

/**
 * authenticate user via PAM
 * username: the username for the current user
 * returns true on success, false otherwise
 */
bool authenticate(const char* username)
{
	const struct pam_conv conv = {
		misc_conv,
		NULL
	};

	pam_handle_t* pam_handle;
	int ret;

	ret = pam_start("gauthctl", username, &conv, &pam_handle);
	if (ret != PAM_SUCCESS)
	{
		fprintf(stderr, "Unable to start PAM conversation: %s\n",
				pam_strerror(pam_handle, ret));
		return false;
	}

	ret = pam_authenticate(pam_handle, 0);
    ret = PAM_SUCCESS;
	if (ret != PAM_SUCCESS)
	{
		fprintf(stderr, "Authentication failed: %s\n",
				pam_strerror(pam_handle, ret));
		return false;
	}

	ret = pam_acct_mgmt(pam_handle, 0);
	if (ret != PAM_SUCCESS)
	{
		fprintf(stderr, "Account unavailable: %s\n",
				pam_strerror(pam_handle, ret));
		return false;
	}

	ret = pam_end(pam_handle, ret);
	if (ret != PAM_SUCCESS)
	{
		fprintf(stderr, "Unable to finish PAM conversation: %s\n",
				pam_strerror(pam_handle, ret));
		return false;
	}
	
	return true;
}

/**
 * allocate a buffer and write the path to the state file to it
 * username: user to write the path for
 * returns an allocated buffer with the path, or NULL on alloc failure
 */
char* get_state_path(const char* username)
{
	/* note: sizeof includes the null terminator */
	const size_t buf_size = (sizeof GAUTH_STATEDIR "/") + strlen(username);
	char* buf = malloc(buf_size);

	if (buf)
	{
		int wr = snprintf(buf, buf_size, "%s/%s", GAUTH_STATEDIR, username);
		if (wr < 0)
		{
			free(buf);
			return NULL;
		}
		assert((size_t) wr < buf_size);
	}
	return buf;
}

/**
 * enable gauth for current user
 * state_path: path to the state file
 * in_fd: file descriptor for the new config
 * returns true on success, false on error
 */
bool enable(const char* state_path, int in_fd)
{
	char* tmp_buf;
	int out_fd;
	int ret;

	/* note: sizeof includes the null terminator */
	const size_t buf_size = strlen(state_path) + (sizeof ".new");
	tmp_buf = malloc(buf_size);
	if (!tmp_buf)
	{
		perror("Memory allocation failed");
		return false;
	}
	else
	{
		int buf_wr = snprintf(tmp_buf, buf_size, "%s.new", state_path);
		if (buf_wr < 0)
		{
			perror("Filename construction failed");
			free(tmp_buf);
			return false;
		}
		assert((size_t) buf_wr < buf_size);
	}

	/* write into a temporary file */
	ret = unlink(tmp_buf);
	if (ret != 0 && errno != ENOENT)
	{
		perror("Unable to pre-unlink temporary file");
		free(tmp_buf);
		return false;
	}

	out_fd = open(tmp_buf, O_WRONLY|O_CREAT|O_EXCL, 0600);
	if (out_fd == -1)
	{
		perror("Unable to open temporary file for writing");
		free(tmp_buf);
		return false;
	}

	while (true)
	{
		char buf[4096];
		ssize_t rd;
		ssize_t wr;

		rd = read(in_fd, buf, sizeof buf);
		if (rd == 0)
			break;
		if (rd == -1)
		{
			perror("Reading config file failed");
			close(out_fd);
			unlink(tmp_buf);
			free(tmp_buf);
			return false;
		}

		wr = write(out_fd, buf, rd);
		if (wr == -1)
		{
			perror("Writing temporary file failed");
			close(out_fd);
			unlink(tmp_buf);
			free(tmp_buf);
			return false;
		}
	}

	close(out_fd);

	/* now we can move the file! */
	ret = rename(tmp_buf, state_path);
	if (ret != 0)
	{
		perror("Replacing state file failed");
		unlink(tmp_buf);
		free(tmp_buf);
		return false;
	}

	free(tmp_buf);
	fputs("GAuth set up successfully\n", stderr);
	return true;
}

/**
 * disable gauth for the current user
 * state_path: path to the state file
 * returns true on success (or if not enabled), false on error
 */
bool disable(const char* state_path)
{
	int ret = unlink(state_path);
	if (ret == 0 || errno == ENOENT)
	{
		fputs("GAuth disabled successfully\n", stderr);
		return true;
	}

	perror("Unable to remove state file");
	return false;
}

int status(const char * filename)
{
    FILE* const out = stdout;
    fprintf(out, "Check existance of %s \n", filename);
    FILE *file;
    if (file = fopen(filename, "r"))
    {
        fclose(file);
        return true;
    }
    return false;
}


int main(int argc, char* argv[])
{
	char opt;
	enum command cmd = CMD_NULL;
	const char* username;
    const char* givenuser;
	char* state_path;
	bool ret;

	while ((opt = getopt_long(argc, argv, "e:d:shV", long_opts, NULL)) != -1)
	{
		switch (opt)
		{
			case 'e':
				cmd = CMD_ENABLE;
				break;
			case 'd':
				cmd = CMD_DISABLE;
                givenuser = optarg;
				break;
			case 's':
				cmd = CMD_STATUS;
				break;
			case 'h':
				return usage(argv[0], true);
			case 'V':
				printf("gauthctl " VERSION "\n");
				return 0;
            case ':': /* missing argument of a parameter */
                fprintf(stderr, "missing argument.\n");
                break;
			default:
				return usage(argv[0], false);
		}
	}

	if (cmd == CMD_NULL || optind != argc)
		return usage(argv[0], false);

	umask(077);

	username = get_user();
	if (!username)
	{
		perror("Unable to get username");
		return 1;
	}

	state_path = get_state_path(username);
	if (!state_path)
	{
		perror("Memory allocation failed");
		return 1;
	}

	/*if (!authenticate(username))
	{
		free(state_path);
		return 1;
	}*/

	assert(cmd > CMD_NULL && cmd < CMD_NEXT);
	switch (cmd)
	{
		case CMD_ENABLE:
            //Enable only possible if 2FA not yet enabled for user
            if (status(state_path) == false)
            {
			    ret = enable(state_path, 3);
            }
            else
            {
                fprintf(stderr, "Error: 2FA configuration exists for user %s.\n",username);
            }
			break;
		case CMD_DISABLE:
            //Only root is allowed to disable 2FA for user
            if (getuid() == 0)
            {    
                state_path = get_state_path(givenuser);
                ret = disable(state_path);
            }
            else
            {
                fprintf(stderr, "Error: Only root is allowed to disable 2FA for user %s.\n",givenuser);
            }
			break;
        case CMD_STATUS:
            ret = status(state_path);
            break;
		case CMD_NULL:
		case CMD_NEXT:
			assert(not_reached);
	}

	free(state_path);

	return ret ? 0 : 1;
}
