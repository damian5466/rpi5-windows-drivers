/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pi5-graph.h"

static HANDLE Device;
static void Fail(const char *message)
{
    fprintf(stderr, "%s (Win32 %lu)\n", message, GetLastError());
    if (Device != INVALID_HANDLE_VALUE) CloseHandle(Device);
    exit(1);
}
static void Query(DWORD code, void *input, DWORD inputSize, void *output, DWORD outputSize)
{
    DWORD used = 0;
    memset(output, 0, outputSize);
    if (!DeviceIoControl(Device, code, input, inputSize, output, outputSize, &used, NULL)) Fail("Graph request failed");
    if (used != outputSize || *(uint32_t *)output != PI5_GRAPH_VERSION) {
        SetLastError(ERROR_INVALID_DATA); Fail("Graph response ABI mismatch");
    }
}
static void String(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    putchar('"');
    while (*s) {
        if (*s == '"' || *s == '\\') { putchar('\\'); putchar(*s); }
        else if (*s < 32) printf("\\u%04x", *s);
        else putchar(*s);
        ++s;
    }
    putchar('"');
}
static void Node(const PI5_GRAPH_NODE *n)
{
    printf("\"Index\":%u,\"Parent\":%u,\"Phandle\":%u,\"PropertyCount\":%u,\"Path\":", n->Index, n->Parent, n->Phandle, n->Properties);
    String(n->Path); printf(",\"Owner\":"); String(n->Owner);
}
static uint32_t Number(const char *s)
{
    char *end;
    unsigned long n;
    errno = 0;
    if (!*s || *s == '-') { SetLastError(ERROR_INVALID_PARAMETER); Fail("Invalid number"); }
    n = strtoul(s, &end, 0);
    if (errno || *end) { SetLastError(ERROR_INVALID_PARAMETER); Fail("Invalid number"); }
    return (uint32_t)n;
}
static void Text(char *out, size_t capacity, const char *text)
{
    if (strlen(text) >= capacity) { SetLastError(ERROR_INVALID_PARAMETER); Fail("Name is too long"); }
    memcpy(out, text, strlen(text) + 1);
}

int main(int argc, char **argv)
{
    PI5_GRAPH_INFO info;
    PI5_GRAPH_NODE node;
    PI5_GRAPH_PROPERTY property;
    PI5_GRAPH_KEY key = {PI5_GRAPH_VERSION, 0, 0, 0};
    uint32_t i, j, at, total;
    Device = INVALID_HANDLE_VALUE;
    if (argc < 2) {
        fprintf(stderr, "Pi5GraphTool status | dump | find path|owner|compatible|phandle text [start] | resolve node property cells-property|- index\n");
        return 2;
    }
    Device = CreateFileW(L"\\\\.\\Pi5Graph", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (Device == INVALID_HANDLE_VALUE) Fail("Open Pi5Graph failed");
    if (argc == 2 && (!strcmp(argv[1], "status") || !strcmp(argv[1], "dump"))) {
        Query(IOCTL_PI5_GRAPH_QUERY, NULL, 0, &info, sizeof(info));
        printf("{\"Version\":%u,\"Schema\":%u,\"Layout\":%u,\"NodeCount\":%u,\"PropertyCount\":%u,\"PropertyBytes\":%u,\"CacheBytes\":%u",
               info.Version, info.Schema, info.Layout, info.Nodes, info.Properties, info.PropertyBytes, info.CacheBytes);
        if (!strcmp(argv[1], "dump")) {
            printf(",\"Nodes\":[");
            for (i = 0; i < info.Nodes; ++i) {
                key.Node = i; key.Index = key.Offset = 0;
                Query(IOCTL_PI5_GRAPH_NODE, &key, sizeof(key), &node, sizeof(node));
                if (i) putchar(',');
                putchar('{'); Node(&node); printf(",\"Properties\":[");
                for (j = 0; j < node.Properties; ++j) {
                    key.Index = j; key.Offset = 0;
                    Query(IOCTL_PI5_GRAPH_PROPERTY, &key, sizeof(key), &property, sizeof(property));
                    if (j) putchar(',');
                    printf("{\"Name\":"); String(property.Name); printf(",\"Hex\":\"");
                    total = property.Total;
                    do {
                        if (property.Total != total || property.Offset != key.Offset ||
                            property.Length > PI5_GRAPH_CHUNK || property.Length > total - key.Offset ||
                            (!property.Length && key.Offset < total)) {
                            SetLastError(ERROR_INVALID_DATA); Fail("Invalid property chunk");
                        }
                        for (at = 0; at < property.Length; ++at) printf("%02x", property.Data[at]);
                        key.Offset += property.Length;
                        if (key.Offset < total) Query(IOCTL_PI5_GRAPH_PROPERTY, &key, sizeof(key), &property, sizeof(property));
                    } while (key.Offset < total);
                    printf("\"}");
                }
                printf("]}");
            }
            putchar(']');
        }
        printf("}\n");
    } else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "find")) {
        PI5_GRAPH_FIND find = {0};
        find.Version = PI5_GRAPH_VERSION;
        if (!strcmp(argv[2], "path")) find.Kind = PI5_GRAPH_FIND_PATH;
        else if (!strcmp(argv[2], "owner")) find.Kind = PI5_GRAPH_FIND_OWNER;
        else if (!strcmp(argv[2], "compatible")) find.Kind = PI5_GRAPH_FIND_COMPATIBLE;
        else if (!strcmp(argv[2], "phandle")) find.Kind = PI5_GRAPH_FIND_PHANDLE;
        else { SetLastError(ERROR_INVALID_PARAMETER); Fail("Invalid find kind"); }
        if (find.Kind == PI5_GRAPH_FIND_PHANDLE) find.Phandle = Number(argv[3]);
        else Text(find.Text, sizeof(find.Text), argv[3]);
        if (argc == 5) find.Start = Number(argv[4]);
        Query(IOCTL_PI5_GRAPH_FIND, &find, sizeof(find), &node, sizeof(node));
        putchar('{'); Node(&node); printf("}\n");
    } else if (argc == 6 && !strcmp(argv[1], "resolve")) {
        PI5_GRAPH_RESOLVE query = {0};
        PI5_GRAPH_REFERENCE reference;
        query.Version = PI5_GRAPH_VERSION; query.Node = Number(argv[2]); query.Index = Number(argv[5]);
        Text(query.Property, sizeof(query.Property), argv[3]);
        if (strcmp(argv[4], "-")) Text(query.CellsProperty, sizeof(query.CellsProperty), argv[4]);
        Query(IOCTL_PI5_GRAPH_RESOLVE, &query, sizeof(query), &reference, sizeof(reference));
        key.Node = reference.Provider;
        Query(IOCTL_PI5_GRAPH_NODE, &key, sizeof(key), &node, sizeof(node));
        printf("{\"Provider\":{"); Node(&node); printf("},\"Cells\":[");
        for (i = 0; i < reference.Count; ++i) printf("%s%u", i ? "," : "", reference.Cells[i]);
        printf("]}\n");
    } else { SetLastError(ERROR_INVALID_PARAMETER); Fail("Invalid command"); }
    CloseHandle(Device);
    return 0;
}
