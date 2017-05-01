/*
 * Copyright (c) 2017 multiplex'd <multiplexd@gmx.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bsd-compat/compat.h"
#include "persist.h"

#ifndef DOAS_STATE_DIR
#define DOAS_STATE_DIR "/var/lib/doas"
#endif

#ifndef DOAS_PERSIST_TIMEOUT
#define DOAS_PERSIST_TIMEOUT 300 /* Five minutes */
#endif

int make_auth_file_path(char *path, char *myname, char *tty) {
   const char *state_dir = DOAS_STATE_DIR;
   char *slash = NULL;
   char tty_clobbered[PATH_MAX];
   char *pathtmp = NULL;

   if (strlcpy(tty_clobbered, tty + 1, sizeof(tty_clobbered)) >= sizeof(tty_clobbered)) {
      return -1;
   }

   /* Replace slashes in the tty name with underscores so we can store
      auth files in the form 'user@tty@ppid'; for example, for user
      joe on /dev/tty3 with shell process id 1285 the auth file will
      be (by default) /var/lib/doas/joe@dev_tty3@1285. */
   while ((slash = strchr(tty_clobbered, '/')) != NULL) {
      *slash = '_';
   }
   
   if (asprintf(&pathtmp, "%s/%s@%s@%d", state_dir, myname, tty_clobbered, getppid()) == -1) {
      return -1;
   }

   if (strlen(pathtmp) > PATH_MAX) {
      free(pathtmp);
      return -1;
   }

   if (strlcpy(path, pathtmp, PATH_MAX) >= PATH_MAX) {
      free(pathtmp);
      return -1;
   }

   free(pathtmp);
   
   return 0;
}

int check_dir(const char *dir) {
   struct stat dirinfo;

   if (stat(dir, &dirinfo) < 0) {
      return -1;
   }

   if (! S_ISDIR(dirinfo.st_mode)) {
      return -1;
   }

   if (dirinfo.st_uid != 0 || dirinfo.st_gid != 0 || dirinfo.st_mode != (S_IRWXU | S_IFDIR)) {
      return -1;
   }

   return 0;
}

/* Return values: -1 on error, 0 on successful auth file access with
   valid token, 1 on successful auth file access with invalid auth
   token. We also make a copy of the path of the auth file when
   returning 1 so we have a path to unlink if the user doesn't
   authenticate to update their auth token */
int persist_check(char *myname, int *authfd, char *path) {
   const char *state_dir = DOAS_STATE_DIR;
   struct stat fileinfo;
   char *tty = NULL;
   char token_file[PATH_MAX];
   int fd;
   time_t now, diff;
   
   if (check_dir(state_dir) == -1) {
      return -1;
   }

   tty = ttyname(STDIN_FILENO);
   if (tty == NULL || strlen(tty) > PATH_MAX) {
      return -1;
   }

   if (make_auth_file_path(token_file, myname, tty) == -1) {
      return -1;
   }

   if (strlcpy(path, token_file, PATH_MAX) >= PATH_MAX) {
      return -1;
   }
   
   /* If the auth file doesn't exist then create it */
   fd = open(token_file, O_RDWR);
   if (fd == -1 && errno == ENOENT) {
      fd = open(token_file, O_RDWR | O_CREAT, 0600);

      if (fd == -1) {
	 return -1;
      }

      *authfd = fd;
      
      /* If we had to create the auth file then there's no
         pre-existing auth token that can be valid */
      return 1;
   }

   if (fstat(fd, &fileinfo) == -1) {
      close(fd);
      return -1;
   }

   /* Make sure that the permissions of the auth token file are what
      we expect */
   if (fileinfo.st_uid != 0 || fileinfo.st_gid != 0 ||
       fileinfo.st_mode != (S_IRUSR | S_IWUSR | S_IFREG)) {

      close(fd);
      return -1;
   }
   
   now = time(NULL);
   if (now == -1) {
      close(fd);
      return -1;
   }

   *authfd = fd;

   /* Check if the auth token is valid */
   diff = now - fileinfo.st_mtim.tv_sec;
   if(diff < 0 || diff > DOAS_PERSIST_TIMEOUT || !(diff <= DOAS_PERSIST_TIMEOUT)) {
      return 1;
   } 
   return 0;
}

/* Force an update of the file's mtime */
void persist_update(int authfd) {
   ftruncate(authfd, 1);
   ftruncate(authfd, 0);
}

int persist_remove(char *myname) {
   const char *state_dir = DOAS_STATE_DIR;
   char *tty = NULL;
   char token_file[PATH_MAX];
   int ret;
   
   if ((tty = ttyname(STDIN_FILENO)) == NULL) {
      return -1;
   }

   if (make_auth_file_path(token_file, myname, tty) == -1) {
      return -1;
   }

   ret = access(token_file, F_OK);
   if (ret == -1 && errno == ENOENT) {
      return 0;
   }
   
   return unlink(token_file);
}
