/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.
*/
#ifndef __CONNECTION_H_
#define __CONNECTION_H_

#include <stdint.h>
#include <sys/types.h>

int    open_connection    (char host[], uint16_t port);
int    open_server_socket (uint16_t port);

/* recv that blocks until receiving exactly `length` bytes */
ssize_t recv_all (int socket, void *buffer, size_t length, int flags);

#endif /* __CONNECTION_H_ */
