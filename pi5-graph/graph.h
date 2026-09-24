/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pi5-graph.h"

#define GRAPH_LIMIT (16u * 1024u * 1024u)
#define GRAPH_ITEMS 32640u
#define GRAPH_RESPONSE 8192u
typedef enum { GraphOk, GraphInvalid, GraphNoMemory, GraphTransport, GraphNotFound } GRAPH_RESULT;
typedef struct { uint16_t Type, Length; const uint8_t *Data; } GRAPH_ARGUMENT;
typedef struct { char *Name; uint8_t *Data; uint32_t Length; } GRAPH_PROPERTY;
typedef struct {
    char *Path, *Owner;
    uint32_t Parent, Count, Phandle;
    GRAPH_PROPERTY *Properties;
} GRAPH_NODE;
typedef struct {
    void *Context;
    void *(*Allocate)(void *context, size_t bytes);
    void (*Free)(void *context, void *allocation);
} GRAPH_MEMORY;
typedef struct {
    void *Context;
    GRAPH_RESULT (*Evaluate)(void *context, uint32_t function,
        uint32_t count, const uint32_t *values, uint8_t *output,
        uint32_t capacity, uint32_t *used);
} GRAPH_SOURCE;
typedef struct {
    GRAPH_MEMORY Memory;
    PI5_GRAPH_INFO Info;
    GRAPH_NODE *Nodes;
} GRAPH;

GRAPH_RESULT GraphDecode(const uint8_t *data, uint32_t size,
                         uint32_t count, GRAPH_ARGUMENT *arguments);
GRAPH_RESULT GraphLoad(GRAPH *graph, const GRAPH_MEMORY *memory, const GRAPH_SOURCE *source);
void GraphFree(GRAPH *graph);
const GRAPH_PROPERTY *GraphProperty(const GRAPH *graph, uint32_t node, const char *name);
GRAPH_RESULT GraphFind(const GRAPH *graph, const PI5_GRAPH_FIND *find, uint32_t *node);
GRAPH_RESULT GraphResolve(const GRAPH *graph, const PI5_GRAPH_RESOLVE *query, PI5_GRAPH_REFERENCE *reference);
