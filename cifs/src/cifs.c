//////////////////////////////////////////////////////////////////////////
///
/// Copyright (c) 2020 Prof. AJ Bieszczad. All rights reserved.
///
//////////////////////////////////////////////////////////////////////////
///
/// This source contains code suitable for testing without FUSE.
///
/// Integration with fuse requires that you must modify the code as needed
/// to work inside the kernel and with the FUSE framework.
///
//////////////////////////////////////////////////////////////////////////

#include "cifs.h"

//////////////////////////////////////////////////////////////////////////
///
/// cifs global variables
///
//////////////////////////////////////////////////////////////////////////

/***

 A file handle of the file system volume residing on a physical device (peripheral).

 File system structure:

 bitvector - one bit per block; i.e., (CIFS_NUMBER_OF_BLOCKS / 8 / CIFS_BLOCK_SIZE) blocks
 superblock - one block
 root descriptor block - one block
 root index block - one block (there might be more if the number of files in the root is large)
 storage blocks (folder, file, data, or index) - all remaining blocks

*/
FILE* cifsVolume;

/***

 A pointer to the in-memory file system context that holds critical information about the volume.

 The in-memory data structures are fast as they reside in the
 computer memory, and not on an external storage device;
 therefore, they are used a fast gateway to the elements of the peripheral volume.

 The context will hold:
 - a copy of the volume superblock
 - a copy of the volume bitvector
 - a hash-table-based registry of all volume files the registry is created when mounting the file system
 - a list of processes currently interacting with cifs.

 All in-memory data must be diligently synchronized with the external volume;
 for example, to find a free block this bitvector MUST be searched (for speed), and if
 a block is allocated (or deallocated), then the block that contains
 the bit flips (and NOT the whole bitvector!) must be copied to the disk.

*/

CIFS_CONTEXT_TYPE* cifsContext;

/***

 FUSE has "context" that holds a number of parameters of the process that accesses it

struct fuse *fuse {
uid_t 	uid,
gid_t 	gid,
pid_t 	pid,
void * 	private_data,
mode_t 	umask }


We need to simulate the context as long as we debug without FUSE, so for convenience we will use
an auxiliary pointer that either points to the real FUSE context or to the simulated
version.

*/

struct fuse_context* fuseContext;
/// must use
// fuseContext = fuse_get_context();
/// when the cifs is integrated with FUSE !!!

//////////////////////////////////////////////////////////////////////////
///
/// cifs function implementations
///
//////////////////////////////////////////////////////////////////////////

/***
 *
 * Allocates space for the file system and saves it to disk.
 *
 * cifsFileName must be either of the three:
 *
 *    1) a regular file
 *    2) a loop interface to a real file
 *    3) a block device name
 *
 */
CIFS_ERROR cifsCreateFileSystem(char* cifsFileName)
{
	// --- create the OS context --- needed here, since writeBlock and cifsReadBlock need the context's superblock

	printf("Size of CIFS_CONTEXT_TYPE: %ld\n", sizeof(CIFS_CONTEXT_TYPE));
	cifsContext = malloc(sizeof(CIFS_CONTEXT_TYPE));
	if (cifsContext == NULL)
		return CIFS_ALLOC_ERROR;

	// open the volume for the file system

	cifsVolume = fopen(cifsFileName, "w+"); //"w+" write and append; just for the creation of the volume
	if (cifsVolume == NULL)
		return CIFS_ALLOC_ERROR;

	// --- put the file system on the volume ---

	// initialize the bitvector

	// allocate space for the in-memory bitvector
	cifsContext->bitvector = calloc(CIFS_NUMBER_OF_BLOCKS / 8, sizeof(char)); // initially all content blocks are free

	// mark as unavailable the blocks used for the bitvector
	memset(cifsContext->bitvector, 0xFF, CIFS_SUPERBLOCK_INDEX / 8);

	// let's postpone writing of the in-memory version of the bitvector to the volume until we also set
	// the bits for the superblock and the two blocks for the root folder

	// initialize the superblock

	cifsContext->superblock = malloc(CIFS_BLOCK_SIZE); // ASSUMES: sizeof(CIFS_SUPERBLOCK_TYPE) <= CIFS_BLOCK_SIZE

	cifsContext->superblock->cifsNextUniqueIdentifier = CIFS_INITIAL_VALUE_OF_THE_UNIQUE_FILE_IDENTIFIER;
	cifsContext->superblock->cifsDataBlockSize = CIFS_BLOCK_SIZE;
	cifsContext->superblock->cifsNumberOfBlocks = CIFS_NUMBER_OF_BLOCKS - 1; // excludes the invalid block number 0xFF
	cifsContext->superblock->cifsRootNodeIndex = CIFS_SUPERBLOCK_INDEX + 1; // root file descriptor is in the next block

    // ...and set the corresponding bit in the bitvector
	cifsSetBit(cifsContext->bitvector, CIFS_SUPERBLOCK_INDEX);

	// initialize the block holding the root folders; there sre two of them: folder descriptor and the index block

	// first, initialize the folder description block
	CIFS_BLOCK_TYPE rootFolderBlock;
	rootFolderBlock.type = CIFS_FOLDER_CONTENT_TYPE;
	// root folder always has "0" as the identifier; it's incremented for the files created later
	rootFolderBlock.content.fileDescriptor.identifier = cifsContext->superblock->cifsNextUniqueIdentifier++;
	rootFolderBlock.content.fileDescriptor.type = CIFS_FOLDER_CONTENT_TYPE;
	strcpy(rootFolderBlock.content.fileDescriptor.name, "/");
	rootFolderBlock.content.fileDescriptor.accessRights = umask(fuseContext->umask);
	rootFolderBlock.content.fileDescriptor.owner = fuseContext->uid;
	rootFolderBlock.content.fileDescriptor.size = 0;
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	rootFolderBlock.content.fileDescriptor.creationTime = time.tv_sec;
	rootFolderBlock.content.fileDescriptor.lastAccessTime = time.tv_sec;
	rootFolderBlock.content.fileDescriptor.lastModificationTime = time.tv_sec;
	rootFolderBlock.content.fileDescriptor.block_ref = cifsContext->superblock->cifsRootNodeIndex + 1; // next block

	// then, initialize the index block of the root folder
	CIFS_BLOCK_TYPE rootFolderIndexBlock;
	rootFolderIndexBlock.type = CIFS_INDEX_CONTENT_TYPE;
	// no files in the root folder yet, so all entries are free
	//memset(&(rootFolderIndexBlock.content), CIFS_INVALID_INDEX, CIFS_INDEX_SIZE);
	for(int i = 0; i < CIFS_INDEX_SIZE; i++) {
		rootFolderIndexBlock.content.index[i] = CIFS_INVALID_INDEX;
	}

	// now, write the two blocks for the root folder and set the corresponding bits in the bitvector
   cifsWriteBlock((const unsigned char *) &rootFolderBlock, cifsContext->superblock->cifsRootNodeIndex);
	cifsSetBit(cifsContext->bitvector, cifsContext->superblock->cifsRootNodeIndex);

   cifsWriteBlock((const unsigned char *) &rootFolderIndexBlock, cifsContext->superblock->cifsRootNodeIndex + 1);
	cifsSetBit(cifsContext->bitvector, cifsContext->superblock->cifsRootNodeIndex + 1);
	
	// write the superblock to the volume
    cifsWriteBlock((const unsigned char *) cifsContext->superblock, CIFS_SUPERBLOCK_INDEX);

	// now we can write the blocks holding the in-memory version of the bitvector to the volume

	unsigned char block[CIFS_BLOCK_SIZE];
	for (int i = 0; i < CIFS_SUPERBLOCK_INDEX; i++) // superblock is the first block after the bitvector
	{
		for (int j = 0; j < CIFS_BLOCK_SIZE; j++)
			block[j] = cifsContext->bitvector[i * CIFS_BLOCK_SIZE + j];

        cifsWriteBlock((const unsigned char *) block, i);
	}

	// create all other blocks
	memset(block, 0, CIFS_BLOCK_SIZE);
	cifsWriteBlock((const unsigned char *) block, CIFS_NUMBER_OF_BLOCKS - 1);

	fflush(cifsVolume);
	fclose(cifsVolume);

	printf("CREATED CIFS VOLUME\n%d bytes\n%d blocks\nBlock size %d bytes\n",
			CIFS_NUMBER_OF_BLOCKS * CIFS_BLOCK_SIZE,
			CIFS_NUMBER_OF_BLOCKS,
			CIFS_BLOCK_SIZE);

	return CIFS_NO_ERROR;
}

/***
 *
 * Loads the file system from a disk and constructs in-memory registry of all files is the system.
 *
 * There are two things to do:
 *    - creating an in-memory hash-table-based registry of the files in the volume
 *    - copying the bitvector from the volume to its in-memory mirror
 *
 * The creation of the registry is realized through the traversal of the whole volume:
 *
 * Starting with the file system root (pointed to from the superblock) traverses the hierarchy of directories
 * and adds an entry for each folder or file to the registry by hashing the name and adding a registry
 * entry node to the conflict resolution list for that entry. If the entry is NULL, the new node will be
 * the only element of that list. If the list contains more than one element, then multiple files hashed to
 * the same value (hash conflict). In this case, the unique file identifier can be used to resolve the conflict.
 * The identifier must be the same as the identifier in the file descriptor pointed to by the node reference.
 *
 * The function must also capture the parenthood relationship by setting the parent file handle field for
 * the file being added to the registry to the file handle of the parent folder (whish is already available since
 * the traversal is top down; i.e., parent folders are always added before their content).
 *
 * The function sets the current working directory to refer to the block holding the root of the volume. This will
 * be changed as the user navigates the file system hierarchy.
 *
 */
CIFS_ERROR cifsMountFileSystem(char* cifsFileName)
{

	cifsVolume = fopen(cifsFileName, "rw+"); // now we will be reading, writing, and appending
    cifsCheckIOError("OPEN", "fopen");

	// --- create the OS context ---

	printf("Size of CIFS_CONTEXT_TYPE: %ld\n", sizeof(CIFS_CONTEXT_TYPE));
	cifsContext = malloc(sizeof(CIFS_CONTEXT_TYPE));
	if (cifsContext == NULL)
		return CIFS_ALLOC_ERROR;

	// get the superblock of the volume
	cifsContext->superblock = (CIFS_SUPERBLOCK_TYPE*) cifsReadBlock(CIFS_SUPERBLOCK_INDEX);

	// get the bitvector of the volume
	cifsContext->bitvector = malloc(cifsContext->superblock->cifsNumberOfBlocks / 8);

	// TODO: read the bitvector from the volume (copying block after block and freeing memory as needed after copying)
	// TODO: NOTE THIS HAS TO BE DONE BEFORE CREATING A FILE!!!

	// create an in-memory registry of the volume

	// TODO: traverse the file system starting with the root and populate the registry

	return CIFS_NO_ERROR;
}

/***
 *
 * Saves the file system to a disk and de-allocates the memory.
 *
 * Assumes that all synchronization has been done.
 *
 */
CIFS_ERROR cifsUmountFileSystem(char* cifsFileName)
{

#ifdef NO_FUSE_DEBUG
	if (fuseContext->fuse != NULL)
		free(fuseContext->fuse);

	if (fuseContext->private_data != NULL)
		free(fuseContext->private_data);

	if (fuseContext != NULL)
		free(fuseContext);
#endif


	// save the current superblock
    cifsWriteBlock((const unsigned char *) cifsContext->superblock, CIFS_SUPERBLOCK_INDEX);
	// note that all bitvector writes need to be done as needed on an ongoing basis as blocks are
	// acquired and released, so any change should have been saved already

	// TODO: make sure that all information is written off to the volume prior to closing it
	// If all is done correctly in other places, then no other writing should be needed
	fflush(cifsVolume);

	fclose(cifsVolume);

	free(cifsContext);

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * Depending on the type parameter the function creates a file or a folder in the current directory
 * of the process.
 *
 * A file can be created only in an open folder, so there must be a corresponding entry
 * in the list of processes. If not, then the function returns CIFS_OPEN_ERROR.
 *
 * Just after creation of the file system, there is only one directory, the root, and it must be opened
 * to create a file in it.
 *
 * If a file with the same name already exists in the current directory, it returns CIFS_DUPLICATE_ERROR.
 *
 * Otherwise:
 *    - sets the folder/file's identifier to the current value of the next unique identifier from the superblock;
 *      then it increments the next available value in the superblock (to prepare it for the next created file)
 *    - finds an available block in the storage using the in-memory bitvector and flips the bit to indicate
 *      that the block is taken
 *    - creates an entry in the conflict resolution list for the corresponding in-memory registry entry
 *      that includes a file descriptor and fills all information about the file in the registry (including
 *      a backpointer to the parent directory that is the reference number of the block holding the file
 *      descriptor of the current working directory)
 *    - copies the local file descriptor to the disk block that was found to be free (the one that will hold
 *      the on-volume file descriptor)
 *    - copies the relevant block of the in-memory bitvector to the corresponding bitevector blocks on the volume
 *
 *  The access rights and the the owner are taken from the context (umask and uid correspondingly).
 *
 */
CIFS_ERROR cifsCreateFile(CIFS_NAME_TYPE filePath, CIFS_CONTENT_TYPE type)
{

CIFS_BLOCK_TYPE freeBlk;
        freeBlk.type = CIFS_FILE_DESCRIPTOR_TYPE;
        // root folder always has "0" as the identifier; it's incremented for the files created later
        freeBlk.content.fileDescriptor.identifier = cifsContext->superblock->cifsNextUniqueIdentifier++;
        freeBlk.content.fileDescriptor.type = CIFS_FILE_CONTENT_TYPE; //used to be cifs content folder type
        strcpy(freeBlk.content.fileDescriptor.name, "file1");
        freeBlk.content.fileDescriptor.accessRights = umask(fuseContext->umask);
        freeBlk.content.fileDescriptor.owner = fuseContext->uid;
        freeBlk.content.fileDescriptor.size = 0;
        struct timespec time;
        clock_gettime(CLOCK_MONOTONIC, &time);
        freeBlk.content.fileDescriptor.creationTime = time.tv_sec;
        freeBlk.content.fileDescriptor.lastAccessTime = time.tv_sec;
        freeBlk.content.fileDescriptor.lastModificationTime = time.tv_sec;
        freeBlk.content.fileDescriptor.block_ref = cifsContext->superblock->freeInBlk + 1; // next block

	CIFS_BLOCK_TYPE freeInBlk;
        freeInBlk.type = CIFS_INDEX_CONTENT_TYPE;
        // no files in the root folder yet, so all entries are free
        //memset(&(rootFolderIndexBlock.content), CIFS_INVALID_INDEX, CIFS_INDEX_SIZE);
        for(int i = 0; i < CIFS_INDEX_SIZE; i++) {
                freeInBlk.content.index[i] = CIFS_INVALID_INDEX;
        }
	cifsContext->superblock = (CIFS_SUPERBLOCK_TYPE*) cifsReadBlock(CIFS_SUPERBLOCK_INDEX);
	return CIFS_NO_ERROR;
}
cifsFindFreeBlock(const unsigned char* bitvector) freeBlk->content.fileDescriptor.block_ref;
cifsFlipBitVector(bitvector);

cifsFindFreeBlock(const unsigned char* bitvector) freeInBlk->content.fileDescriptor.block_ref;
cifsFlipBitVector(bitvector);

cifsReadBlock((const unsigned char *) cifsContext->freeBlk, CIFS_SUPERBLOCK_INDEX); //read fileblock into superblock
cifsReadBlock((const unsigned char *) cifsContext->freeInBlk, CIFS_SUPERBLOCK_INDEX); // read root fd into superblock

cifsWriteBlock((const unsigned char *) &freeBlk, cifsContext->superblock->freeInBlk); // this goes at the end of the code because we are saving it
cifsSetBit(cifsContext->bitvector, cifsContext->superblock->freeInBlk);

//////////////////////////////////////////////////////////////////////////

/***
 * 
 * Deletes a file from the filesystem
 * 
 * Hashes the file nameto check if the file is in the registry, if not, return CIFS_NOT_FOUND_ERROR
 * 
 * If the file is found, but the reference count is non-zero (that means a process has the file opened),
 * then return CIFS_IN_USE_ERROR
 * 
 * Otherwise,
 * - if it is a non-empty folder, return CIFS_NOT_EMPTY_ERROR
 * - check if process owner has write permission to file or folder, if not, return CIFS_ACCESS_ERROR
 * - free all blocks belonging to the file by flipping the correpsonding bits in the in-memory bitvector
 *    - data blocks, index block(s), file descriptor block
 *    - note, you don't have to clear the data
 * - clear the entry in the parent folder's index
 * - decrement the size of the parent's folder
 * - write the parent folder's file descriptor and index block to the disk
 *    - update the in-memory registry with the new information
 * - write the bitvector to the volume
 * 
*/
CIFS_ERROR cifsDeleteFile(CIFS_NAME_TYPE filePath)
{
	// TODO: implement

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * Opens a file for reading or writing.
 *
 * If the file is not found in the in-memory registry, then the function returns CIFS_NOT_FOUND_ERROR (since only
 * existing files can be opened).
 *
 * Checks if the file is already open by any process by examining the list of processes in the context.
 * If so, then the function returns CIFS_OPEN_ERROR.
 *
 * Otherwise, a check is made whether the user can be granted the desired access. For that the file descriptor
 * that holds the user id of the owner and the file access rights must be examined and compared with the user id
 * of the owner of the process calling this function (available in the FUSE context) and the desired access passed
 * in the parameter. If the user is not granted the desired access, then the function returns CIFS_ACCESS_ERROR.
 *
 * Otherwise, an entry for the process is added to the list of processes with the file handle and the granted
 * access rights for the process. Each access to the file (read, write, get info) will need to verify that
 * the requested operation is allowed for the requesting user.
 *
 * Finally, the function increments the reference count for the file, sets the output parameter to the handle
 * for the file and returns CIFS_NO_ERROR.
 *
 */
CIFS_ERROR cifsOpenFile(CIFS_NAME_TYPE filePath, mode_t desiredAccessRights, CIFS_FILE_HANDLE_TYPE *fileHandle)
{
	// TODO: implement

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * The function closes the file with the specified file handle for a specific process (obtained through
 * the FUSE context).
 *
 * If the process does not have the file with the passed file handle opened (i.e., it is not an element
 * of the process list with the file passed handle), then the function returns CIFS_ACCESS_ERROR.
 *
 * Removes the entry for the process and the file from the list of processes, and then decreases the reference
 * count for the file.
 *
 */
CIFS_ERROR cifsCloseFile(CIFS_FILE_HANDLE_TYPE fileHandle)
{
	// TODO: implement

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * Finds the file in the in-memory registry and obtains the information about the file from the file descriptor
 * block referenced from the registry.
 *
 * If the file is not found, then it returns CIFS_NOT_FOUND_ERROR
 *
 */
CIFS_ERROR cifsGetFileInfo(CIFS_NAME_TYPE filePath, CIFS_FILE_DESCRIPTOR_TYPE* infoBuffer)
{
	// TODO: implement
	
	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * The function replaces content of a file with new one pointed to by the parameter writeBuffer.
 *
 * The file must be opened by the process trying to write, so the process must have a file handle of the file
 * while attempting this operation.
 *
 * Otherwise, it checks the access rights for writing by examining the entry for the process and the file
 * in the list of processes. If the process owner (from the fuse context) is not allowed
 * to write to the file, then the function returns CIFS_ACCESS_ERROR.
 *
 * Then, the function calculates the space needed for the new content and checks if the write buffer can fit into
 * the remaining free space in the file system. If not, then the CIFS_ALLOC_ERROR is returned.
 *
 * Otherwise, the function:
 *    - acquires as many new blocks as needed to hold the new content modifying the corresponding bits in
 *      the in-memory bitvector,
 *    - copies the characters pointed to by the parameter writeBuffer (until '\0' but excluding it) to the
 *      new just acquired blocks,
 *    - copies any modified block of the in-memory bitvector to the corresponding bitvector block on the disk.
 *
 * If the new content has been written successfully, the function then removes all blocks currently held by
 * this file and modifies the file descriptor to reflect the new location, the new size of the file, and the new
 * times of last modification and access.
 *
 * This order of actions prevents file corruption, since in case of any error with writing new content, the file's
 * old version is intact. This technique is called copy-on-write and is an alternative to journalling.
 *
 * The content of the in-memory file descriptor must be replaced by the new data.
 *
 * The function returns CIFS_WRITE_ERROR in response to exception not specified earlier.
 *
 */
CIFS_ERROR cifsWriteFile(CIFS_FILE_HANDLE_TYPE fileHandle, char* writeBuffer)
{
	// TODO: implement

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////

/***
 *
 * The function returns the complete content of the file to the caller through the parameter readBuffer.
 *
 * The file must be opened by the process trying to read, so the process must have a file handle of the file
 * while attempting this operation.
 *
 * Otherwise, it checks the access rights for reading by examining the entry for the process and the file
 * in the list of processes. If the process owner is not allowed to read from the file, then the function
 * returns CIFS_ACCESS_ERROR.
 *
 * Otherwise, the function allocates memory sufficient to hold the read content with an appended end of string
 * character; the pointer to newly allocated memory is passed back through the readBuffer parameter. All the content
 * of the blocks is concatenated using the allocated space, and an end of string character is appended at the end of
 * the concatenated content.
 *
 * The function returns CIFS_READ_ERROR in response to exception not specified earlier.
 *
 */
CIFS_ERROR cifsReadFile(CIFS_FILE_HANDLE_TYPE fileHandle, char** readBuffer)
{
	// TODO: implement

	return CIFS_NO_ERROR;
}

//////////////////////////////////////////////////////////////////////////
///
/// Functions to write and read block to and from block devices
///
//////////////////////////////////////////////////////////////////////////

void cifsCheckIOError(const char* who, const char* what)
{
	int errCode = ferror(cifsVolume);
	if (errCode == 0)
		return;
	printf("%s: %s returned \"%s\"\n", who, what, strerror(errCode));
	exit(errCode);
}

void cifsPrintBlockContent(const unsigned char *str)
{
	for (int i=0; i < CIFS_BLOCK_SIZE; i++)
		printf("0x%02x ", *(str + i));
}

/***
 *
 * Write a single block to the block device.
 *
 */
size_t cifsWriteBlock(const unsigned char* content, CIFS_INDEX_TYPE blockNumber)
{
	fseek(cifsVolume, blockNumber * CIFS_BLOCK_SIZE, SEEK_SET);
    cifsCheckIOError("WRITE", "fseek");
	printf("WRITE: POSITION=%6ld, ", ftell(cifsVolume));
    cifsCheckIOError("WRITE", "ftell");
	size_t len = fwrite((const void *)content, sizeof(unsigned char), CIFS_BLOCK_SIZE, cifsVolume);
    cifsCheckIOError("WRITE", "fwrite");
	printf("LENGTH=%4ld, CONTENT=", len); // %s will ussually not work
	cifsPrintBlockContent(content);
	printf("\n");

	return len;
}

/***
 *
 * Read a single block from a block device.
 *
 */
unsigned char* cifsReadBlock(CIFS_INDEX_TYPE blockNumber)
{
	unsigned char* content = malloc(CIFS_BLOCK_SIZE);

	fseek(cifsVolume, blockNumber * CIFS_BLOCK_SIZE, SEEK_SET);
    cifsCheckIOError("READ", "fseek");
	printf("READ : POSITION=%6ld, ", ftell(cifsVolume));
    cifsCheckIOError("READ", "ftell");
	size_t len = fread((void * restrict)content, sizeof(unsigned char), CIFS_BLOCK_SIZE, cifsVolume);
    cifsCheckIOError("READ", "fread");
	printf("LENGTH=%4ld, CONTENT=", len); // %s will usually not work
	cifsPrintBlockContent(content);
	printf("\n");

	return content;
}

//////////////////////////////////////////////////////////////////////////
///
/// some helper functions
///
//////////////////////////////////////////////////////////////////////////

/***
 *
 * Returns a hash value within the limits of the registry.
 *
 */

inline unsigned long hash(const char* str)
{

	register unsigned long hash = 5381;
	register unsigned char c;

	while ((c = *str++) != '\0')
		hash = ((hash << 5) + hash) ^ c; /* hash * 33 + c */

	return hash % CIFS_REGISTRY_SIZE;
}

/***
 *
 * Find a free block in a bit vector.
 *
 */
inline CIFS_INDEX_TYPE cifsFindFreeBlock(const unsigned char* bitvector)
{

	unsigned int i = 0;
	while (bitvector[i] == 0xFF)
		i += 1;

	register unsigned char mask = 0x80;
	CIFS_INDEX_TYPE j = 0;
	while (bitvector[i] & mask)
	{
		mask >>= 1;
		++j;
	}

	return (i * 8) + j; // i bytes and j bits are all "1", so this formula points to the first "0"
}

/***
 *
 * Three functions for bit manipulation.
 *
 */
inline void cifsFlipBit(unsigned char* bitvector, CIFS_INDEX_TYPE bitIndex)
{

	CIFS_INDEX_TYPE blockIndex = bitIndex / 8;
	CIFS_INDEX_TYPE bitShift = bitIndex % 8;

	register unsigned char mask = 0x80;
	bitvector[blockIndex] ^= (mask >> bitShift);
}

inline void cifsSetBit(unsigned char* bitvector, CIFS_INDEX_TYPE bitIndex)
{

	CIFS_INDEX_TYPE blockIndex = bitIndex / 8;
	CIFS_INDEX_TYPE bitShift = bitIndex % 8;

	register unsigned char mask = 0x80;
	bitvector[blockIndex] |= (mask >> bitShift);
}

inline void cifsClearBit(unsigned char* bitvector, CIFS_INDEX_TYPE bitIndex)
{

	CIFS_INDEX_TYPE blockIndex = bitIndex / 8;
	CIFS_INDEX_TYPE bitShift = bitIndex % 8;

	register unsigned char mask = 0x80;
	bitvector[blockIndex] &= ~(mask >> bitShift);
}

/***
 *
 * Generates random readable/printable content for testing
 *
 */

char* cifsGenerateContent(int size)
{

	size = (size <= 0 ? rand() % 1000 : size); // arbitrarily chosen as an example

	char* content = malloc(size);

	int firstPrintable = ' ';
	int len = '~' - firstPrintable;

	for (int i = 0; i < size - 1; i++)
		*(content + i) = firstPrintable + rand() % len;

	content[size - 1] = '\0';
	return content;
}


/*******
 * Some extra helper functions
 */
int doesFileExist(char* filePath) {
	// TODO: implement
	return CIFS_NO_ERROR;
}

void traverseDisk(CIFS_INDEX_TYPE* index, int size, char* path) {
	// TODO: implement
}

void addToHashTable(long index, char* filePath, CIFS_FILE_DESCRIPTOR_TYPE* fd)
{
	// TODO: implement
}

void writeBvSb(void) {
	// TODO: implement

	// write bitvector (Bv)
	
	// write superblock (Sb)
	
}
