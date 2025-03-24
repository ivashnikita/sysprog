#include "userfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;

	size_t size;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;

	enum open_flags mode;

	int current_block;
	int current_position;
};

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */

static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

int
is_incorrect_fd(const int fd) {
	return fd < 0 || fd >= file_descriptor_capacity || !file_descriptors[fd];
}

int
does_have_read_permission(const int fd) {
	struct filedesc *fdesc = file_descriptors[fd];
	enum open_flags mode = fdesc->mode;

	return mode == UFS_READ_WRITE || mode == UFS_READ_ONLY;
}

int
does_have_write_permission(const int fd) {
	struct filedesc *fdesc = file_descriptors[fd];
	enum open_flags mode = fdesc->mode;

	return mode == UFS_READ_WRITE || mode == UFS_WRITE_ONLY;
}

void
resize_file_descriptors() {
	if (file_descriptor_capacity == 0) {
		file_descriptor_capacity = 2;
		file_descriptors = malloc(file_descriptor_capacity * sizeof(struct filedesc*));
	}

	if (file_descriptor_count == file_descriptor_capacity) {
		file_descriptor_capacity *= 2;
		struct filedesc **new_file_descriptors = realloc(file_descriptors, file_descriptor_capacity * sizeof(struct filedesc*));
		file_descriptors = new_file_descriptors;
	}
}

int
create_fd(struct file* file, enum open_flags mode) {
	struct filedesc *fd = malloc(sizeof(struct filedesc));

	fd->mode = mode;

	fd->current_block = 0;
	fd->current_position = 0;

	fd->file = file;
	fd->file->refs++;

	for (int i = 0; i < file_descriptor_count; i++) {
		if (!file_descriptors[i]) {
			file_descriptors[i] = fd;

			return i;
		}
	}

	resize_file_descriptors();

	file_descriptors[file_descriptor_count] = fd;
	file_descriptor_count++;

	return file_descriptor_count - 1;
}

int
delete_fd(int fd) {
	if (is_incorrect_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;

		return -1;
	}

	struct filedesc *fdesc = file_descriptors[fd];
	fdesc->file->refs--;
	free(fdesc);
	file_descriptors[fd] = NULL;

	return 0;
}

struct file*
get_file(const char *filename) {
	if (!file_list) {
		return NULL;
	}

	struct file *current = file_list;

	while (current) {
		if (strcmp(current->name, filename) == 0) {
			return current;
		}

		current = current->next;
	}

	return NULL;
}


void
file_list_add(struct file *file) {
	struct file *current = file_list;

	if (!current) {
		file_list = file;
		file->prev = NULL;
		file->next = NULL;
		return;
	}

	while (current->next) {
		current = current->next;
	}

	file->prev = current;
	current->next = file;
	file->next = NULL;
}

int
ufs_open(const char *filename, int flags)
{
	if (flags != UFS_CREATE) {
		struct file *file = get_file(filename);

		if (!file) {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}

		if (flags == 0) {
			return create_fd(file, UFS_READ_WRITE);
		}

		return create_fd(file, flags);
	}

	struct file *file = get_file(filename);

	if (file) {
		return create_fd(file, UFS_READ_WRITE);
	}

	file = malloc(sizeof(struct file));
	struct block *block = malloc(sizeof(struct block));

	block->occupied = 0;
	block->memory = malloc(BLOCK_SIZE);
	block->prev = NULL;
	block->next = NULL;

	file->block_list = block;
	file->last_block = block;
	file->size = 0;

	file->name = malloc(strlen(filename) + 1);
	strcpy(file->name, filename);

	file_list_add(file);

	return create_fd(file, UFS_READ_WRITE);
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (is_incorrect_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;

		return -1;
	}

	if (!does_have_write_permission(fd)) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;

		return -1;
	}

	size_t written_data_size = 0;

	struct filedesc *fd_info = file_descriptors[fd];
	struct file *file = fd_info->file;
	struct block *block = file->block_list;

	if (fd_info->current_block * BLOCK_SIZE + fd_info->current_position + size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	for (int i = 0; i < fd_info->current_block; i++) {
		block = block->next;
	}

	while (written_data_size < size) {
		size_t left_space = BLOCK_SIZE - fd_info->current_position;
		size_t write_size = (size - written_data_size < left_space) ? size - written_data_size : left_space;

		if (fd_info->current_block * BLOCK_SIZE + fd_info->current_position + write_size >= MAX_FILE_SIZE) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}

		memcpy(block->memory + fd_info->current_position, buf + written_data_size, write_size);
		fd_info->current_position += (int)write_size;
		if (fd_info->current_position > block->occupied) {
			file->size -= block->occupied;
			block->occupied = fd_info->current_position;
			file->size += fd_info->current_position;
		}

		written_data_size += write_size;

		if (block->next) {
			block = block->next;
			fd_info->current_block++;
			fd_info->current_position = 0;
		} else {
			break;
		}
	}

	while (written_data_size < size) {
		fd_info->current_block++;
		fd_info->current_position = 0;
		struct block *new_last_block = malloc(sizeof(struct block));

		block->next = new_last_block;
		new_last_block->prev = block;

		new_last_block->occupied = 0;
		new_last_block->memory = malloc(BLOCK_SIZE);
		new_last_block->next = NULL;

		file->last_block = new_last_block;

		size_t write_size = (size - written_data_size < BLOCK_SIZE) ? size - written_data_size : BLOCK_SIZE;

		if (fd_info->current_block * BLOCK_SIZE + fd_info->current_position + write_size > MAX_FILE_SIZE) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}

		memcpy(new_last_block->memory, buf + written_data_size, write_size);
		fd_info->current_position += (int)write_size;
		new_last_block->occupied += (int)write_size;

		file->size += write_size;
		written_data_size += write_size;

		block = block->next;
	}

	return (ssize_t)written_data_size;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (is_incorrect_fd(fd)) {
		ufs_error_code = UFS_ERR_NO_FILE;

		return -1;
	}

	if (!does_have_read_permission(fd)) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;

		return -1;
	}

	size_t read_data_size = 0;

	struct filedesc *fd_info = file_descriptors[fd];
	struct file *file = fd_info->file;
	struct block *block = file->block_list;

	for (int i = 0; i < fd_info->current_block; i++) {
		block = block->next;
	}

	if (fd_info->current_position == block->occupied && !block->next) {
		return (ssize_t)read_data_size;
	}

	if (read_data_size < size) {
		size_t available_size = block->occupied - fd_info->current_position;
		size_t read_size = size < available_size ? size : available_size;

		memcpy(buf, block->memory + fd_info->current_position, read_size);
		fd_info->current_position += (int)read_size;
		read_data_size += read_size;

		if (read_data_size < size && block->next) {
			block = block->next;
			fd_info->current_block++;
			fd_info->current_position = 0;
		}
	}

	if (fd_info->current_position == block->occupied && !block->next) {
		return (ssize_t)read_data_size;
	}

	while (read_data_size < size) {
		int read_size = ((int)(size - read_data_size) < block->occupied) ? (int)(size - read_data_size) : block->occupied;

		memcpy(buf + read_data_size, block->memory, read_size);
		fd_info->current_position += (int)read_size;
		read_data_size += read_size;

		if (block->occupied < BLOCK_SIZE) {
			break;
		}

		if (read_data_size < size && block->next) {
			block = block->next;
			fd_info->current_block++;
			fd_info->current_position = 0;
		}
	}

	return (ssize_t)read_data_size;
}

int
ufs_close(int fd)
{
	return delete_fd(fd);
}

int
ufs_delete(const char *filename)
{
	struct file *file = get_file(filename);

	if (!file) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (!file->prev && !file->next) {
		file_list = NULL;
	} else if (!file->prev) {
		file_list = file->next;
		file->next->prev = NULL;
	} else if (!file->next) {
		file->prev->next = NULL;
	} else {
		file->prev->next = file->next;
		file->next->prev = file->prev;
	}

	struct block *current_block = file->block_list;
	while (current_block != NULL) {
		struct block *next_block = current_block->next;

		free(current_block->memory);
		free(current_block);

		current_block = next_block;
	}

	free(file->name);
	free(file);

	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)fd;
	(void)new_size;
	ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
	return -1;
}

#endif

void
ufs_destroy(void)
{
	struct file *file = file_list;

	while (file) {
		struct block *block = file->block_list;

		while (block) {
			free(block->memory);
			struct block *buf = block;
			block = block->next;
			free(buf);
		}

		free(file->name);

		struct file *buf = file;
		file = file->next;
		free(buf);
	}

	for (int i = 0; i < file_descriptor_count; i++) {
		struct filedesc *fd = file_descriptors[i];
		free(fd);
	}

	free(file_descriptors);
}
