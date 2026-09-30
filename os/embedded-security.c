/* Descriptor-free embedding security policy.
 *
 * In-process clients are admitted by the embedding host. There is no peer
 * address, authorization file, or mutable network host list in this product.
 */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include "dixstruct.h"
#include "os.h"

Bool defeatAccessControl = FALSE;

int
AddHost(ClientPtr client, int family, unsigned length, const void *address)
{
    (void)client;
    (void)family;
    (void)length;
    (void)address;
    return Success;
}

int
RemoveHost(ClientPtr client, int family, unsigned length, pointer address)
{
    (void)client;
    (void)family;
    (void)length;
    (void)address;
    return Success;
}

int
GetHosts(pointer *data, int *count, int *length, BOOL *enabled)
{
    *data = NULL;
    *count = 0;
    *length = 0;
    *enabled = FALSE;
    return Success;
}

int
InvalidHost(sockaddrPtr address, int length, ClientPtr client)
{
    (void)address;
    (void)length;
    (void)client;
    return 0;
}

int
LocalClient(ClientPtr client)
{
    (void)client;
    return TRUE;
}

int
LocalClientCred(ClientPtr client, int *uid, int *gid)
{
    (void)client;
    (void)uid;
    (void)gid;
    return -1;
}

int
ChangeAccessControl(ClientPtr client, int enabled)
{
    (void)client;
    (void)enabled;
    return Success;
}

void AddLocalHosts(void) {}
void ResetHosts(const char *displayName) { (void)displayName; }
void EnableLocalHost(void) {}
void DisableLocalHost(void) {}
void AccessUsingXdmcp(void) {}
void DefineSelf(int fd) { (void)fd; }
void AugmentSelf(pointer address, int length)
{
    (void)address;
    (void)length;
}

void InitAuthorization(char *filename) { (void)filename; }
void RegisterAuthorizations(void) {}
void ResetAuthorization(void) {}

XID
CheckAuthorization(unsigned int nameLength, const char *name,
                   unsigned int dataLength, const char *data,
                   ClientPtr client, const char **reason)
{
    (void)nameLength;
    (void)name;
    (void)dataLength;
    (void)data;
    (void)client;
    if (reason)
        *reason = NULL;
    return 0;
}

int
AddAuthorization(unsigned int nameLength, const char *name,
                 unsigned int dataLength, char *data)
{
    (void)nameLength;
    (void)name;
    (void)dataLength;
    (void)data;
    return 0;
}

XID
GenerateAuthorization(unsigned int nameLength, char *name,
                      unsigned int dataLength, char *data,
                      unsigned int *resultLength, char **result)
{
    (void)nameLength;
    (void)name;
    (void)dataLength;
    (void)data;
    if (resultLength)
        *resultLength = 0;
    if (result)
        *result = NULL;
    return (XID)~0L;
}
