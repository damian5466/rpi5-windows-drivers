/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "graph.h"
#include <string.h>

static uint32_t Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t Be32(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

/* ACPI V1 arguments need not be aligned. No raw ACPI pointers escape this
 * parser. The graph methods return flattened scalar arguments, not packages. */
GRAPH_RESULT GraphDecode(const uint8_t *data, uint32_t size,
                         uint32_t count, GRAPH_ARGUMENT *arguments)
{
    uint32_t at = 12, i, width;
    if (size < 12 || Le32(data) != 0x426f6541u || Le32(data + 4) != size ||
        Le32(data + 8) != count || count > 4) return GraphInvalid;
    for (i = 0; i < count; ++i) {
        GRAPH_ARGUMENT *a = &arguments[i];
        if (size - at < 8) return GraphInvalid;
        a->Type = (uint16_t)(data[at] | (uint16_t)data[at + 1] << 8);
        a->Length = (uint16_t)(data[at + 2] | (uint16_t)data[at + 3] << 8);
        width = a->Length < 4 ? 4u : a->Length;
        if (width > size - at - 4) return GraphInvalid;
        a->Data = data + at + 4;
        if (a->Type > 2 ||
            (a->Type == 0 && a->Length != 4 && a->Length != 8) ||
            (a->Type == 1 && (a->Length == 0 || a->Data[a->Length - 1] != 0 ||
             memchr(a->Data, 0, a->Length - 1) != NULL))) return GraphInvalid;
        at += 4 + width;
    }
    return at == size ? GraphOk : GraphInvalid;
}

static int Integer(const GRAPH_ARGUMENT *a, uint32_t *value)
{
    if (a->Type != 0 || (a->Length == 8 && Le32(a->Data + 4) != 0)) return 0;
    *value = Le32(a->Data);
    return 1;
}

static void *Allocate(GRAPH *graph, size_t size)
{
    void *p;
    if (!size || size > GRAPH_LIMIT - graph->Info.CacheBytes) return NULL;
    p = graph->Memory.Allocate(graph->Memory.Context, size);
    if (p) {
        memset(p, 0, size);
        graph->Info.CacheBytes += (uint32_t)size;
    }
    return p;
}

static char *CopyString(GRAPH *graph, const GRAPH_ARGUMENT *a, uint32_t limit)
{
    char *p;
    if (a->Type != 1 || a->Length > limit) return NULL;
    p = Allocate(graph, a->Length);
    if (p) memcpy(p, a->Data, a->Length);
    return p;
}

void GraphFree(GRAPH *graph)
{
    uint32_t i, j;
    if (graph->Nodes) {
        for (i = 0; i < graph->Info.Nodes; ++i) {
            GRAPH_NODE *n = &graph->Nodes[i];
            if (n->Properties) {
                for (j = 0; j < n->Count; ++j)
                    if (n->Properties[j].Name)
                        graph->Memory.Free(graph->Memory.Context, n->Properties[j].Name);
                graph->Memory.Free(graph->Memory.Context, n->Properties);
            }
            if (n->Path) graph->Memory.Free(graph->Memory.Context, n->Path);
            if (n->Owner) graph->Memory.Free(graph->Memory.Context, n->Owner);
        }
        graph->Memory.Free(graph->Memory.Context, graph->Nodes);
    }
    memset(graph, 0, sizeof(*graph));
}

const GRAPH_PROPERTY *GraphProperty(const GRAPH *graph, uint32_t node, const char *name)
{
    uint32_t i;
    if (node >= graph->Info.Nodes) return NULL;
    for (i = 0; i < graph->Nodes[node].Count; ++i)
        if (!strcmp(graph->Nodes[node].Properties[i].Name, name))
            return &graph->Nodes[node].Properties[i];
    return NULL;
}

static GRAPH_RESULT Evaluate(const GRAPH_SOURCE *source, uint8_t *buffer,
    uint32_t function, uint32_t count, const uint32_t *values,
    uint32_t expected, GRAPH_ARGUMENT *arguments)
{
    uint32_t used = 0;
    GRAPH_RESULT r = source->Evaluate(source->Context, function, count, values,
                                      buffer, GRAPH_RESPONSE, &used);
    if (r != GraphOk) return r;
    if (used > GRAPH_RESPONSE) return GraphInvalid;
    return GraphDecode(buffer, used, expected, arguments);
}

static GRAPH_RESULT Load(GRAPH *g, const GRAPH_SOURCE *source, uint8_t *buffer)
{
    GRAPH_ARGUMENT a[4];
    GRAPH_RESULT r;
    uint32_t i, j, k, values[3], total, offset, chunk, value, parent, depth;
    r = Evaluate(source, buffer, 0, 0, NULL, 1, a);
    if (r != GraphOk) return r;
    if (a[0].Type != 2 || a[0].Length < 1 || (a[0].Data[0] & 15) != 15) return GraphInvalid;
    r = Evaluate(source, buffer, 1, 0, NULL, 3, a);
    if (r != GraphOk) return r;
    if (!Integer(&a[0], &g->Info.Schema) || g->Info.Schema != 1 ||
        !Integer(&a[1], &g->Info.Nodes) || !g->Info.Nodes || g->Info.Nodes > GRAPH_ITEMS ||
        !Integer(&a[2], &g->Info.Layout) || g->Info.Layout > 1) return GraphInvalid;
    g->Nodes = Allocate(g, sizeof(*g->Nodes) * g->Info.Nodes);
    if (!g->Nodes) return GraphNoMemory;
    for (i = 0; i < g->Info.Nodes; ++i) {
        GRAPH_NODE *n = &g->Nodes[i];
        values[0] = i;
        r = Evaluate(source, buffer, 2, 1, values, 4, a);
        if (r != GraphOk) return r;
        if (a[0].Type != 1 || a[0].Length > PI5_GRAPH_PATH || a[0].Data[0] != '/' ||
            a[2].Type != 1 || a[2].Length > PI5_GRAPH_PATH ||
            (a[2].Data[0] && a[2].Data[0] != '\\') ||
            !Integer(&a[1], &n->Parent) || !Integer(&a[3], &n->Count) ||
            n->Count > GRAPH_ITEMS - g->Info.Properties ||
            (i == 0 ? (n->Parent != PI5_GRAPH_NONE || strcmp((const char *)a[0].Data, "/")) : n->Parent >= i)) return GraphInvalid;
        g->Info.Properties += n->Count;
        n->Path = CopyString(g, &a[0], PI5_GRAPH_PATH);
        n->Owner = CopyString(g, &a[2], PI5_GRAPH_PATH);
        if (!n->Path || !n->Owner) return GraphNoMemory;
        for (k = 0; k < i; ++k) if (!strcmp(g->Nodes[k].Path, n->Path)) return GraphInvalid;
        if (i) {
            const char *p = g->Nodes[n->Parent].Path;
            size_t length = strlen(p);
            const char *child;
            if (strlen(n->Path) <= length || strncmp(n->Path, p, length) ||
                (n->Parent && n->Path[length] != '/')) return GraphInvalid;
            child = n->Path + (n->Parent ? length + 1 : 1);
            if (!*child || strchr(child, '/')) return GraphInvalid;
        }
        parent = n->Parent;
        for (depth = 0; parent != PI5_GRAPH_NONE; parent = g->Nodes[parent].Parent)
            if (++depth > 64) return GraphInvalid;
        if (n->Count) {
            n->Properties = Allocate(g, sizeof(*n->Properties) * n->Count);
            if (!n->Properties) return GraphNoMemory;
        }
        for (j = 0; j < n->Count; ++j) {
            GRAPH_PROPERTY *p = &n->Properties[j];
            values[1] = j;
            offset = 0;
            do {
                values[2] = offset;
                r = Evaluate(source, buffer, 3, 3, values, 3, a);
                if (r != GraphOk) return r;
                if (a[0].Type != 1 || a[0].Length < 2 || a[0].Length > PI5_GRAPH_PROPERTY_NAME ||
                    !Integer(&a[1], &total) || total > GRAPH_LIMIT) return GraphInvalid;
                if (!offset) {
                    p->Name = Allocate(g, (size_t)a[0].Length + total);
                    if (!p->Name) return GraphNoMemory;
                    memcpy(p->Name, a[0].Data, a[0].Length);
                    p->Data = (uint8_t *)p->Name + a[0].Length;
                    p->Length = total;
                    g->Info.PropertyBytes += total;
                    for (k = 0; k < j; ++k)
                        if (!strcmp(n->Properties[k].Name, p->Name)) return GraphInvalid;
                } else if (total != p->Length || strcmp((const char *)a[0].Data, p->Name)) return GraphInvalid;
                if (a[2].Type == 2) chunk = a[2].Length;
                else if (Integer(&a[2], &value) && value == 0 && offset == total) chunk = 0;
                else return GraphInvalid;
                if (chunk > PI5_GRAPH_CHUNK || chunk > total - offset || (!chunk && offset != total)) return GraphInvalid;
                if (chunk) memcpy(p->Data + offset, a[2].Data, chunk);
                offset += chunk;
            } while (offset < p->Length);
        }
    }
    for (i = 0; i < g->Info.Nodes; ++i) {
        const GRAPH_PROPERTY *p = GraphProperty(g, i, "phandle");
        const GRAPH_PROPERTY *linuxPhandle = GraphProperty(g, i, "linux,phandle");
        if (p && linuxPhandle && (p->Length != linuxPhandle->Length || memcmp(p->Data, linuxPhandle->Data, p->Length))) return GraphInvalid;
        if (!p) p = linuxPhandle;
        if (!p) continue;
        if (p->Length != 4 || (value = Be32(p->Data)) == 0 || value == UINT32_MAX) return GraphInvalid;
        g->Nodes[i].Phandle = value;
        for (j = 0; j < i; ++j) if (g->Nodes[j].Phandle == value) return GraphInvalid;
    }
    return GraphOk;
}

GRAPH_RESULT GraphLoad(GRAPH *graph, const GRAPH_MEMORY *memory, const GRAPH_SOURCE *source)
{
    GRAPH_RESULT r;
    uint8_t *buffer;
    memset(graph, 0, sizeof(*graph));
    graph->Memory = *memory;
    graph->Info.Version = PI5_GRAPH_VERSION;
    buffer = memory->Allocate(memory->Context, GRAPH_RESPONSE);
    if (!buffer) return GraphNoMemory;
    r = Load(graph, source, buffer);
    memory->Free(memory->Context, buffer);
    if (r != GraphOk) GraphFree(graph);
    return r;
}

GRAPH_RESULT GraphFind(const GRAPH *g, const PI5_GRAPH_FIND *find, uint32_t *node)
{
    uint32_t i;
    if (find->Version != PI5_GRAPH_VERSION || find->Kind < 1 || find->Kind > 4 ||
        !memchr(find->Text, 0, sizeof(find->Text)) ||
        (find->Kind != PI5_GRAPH_FIND_PHANDLE && !find->Text[0]) ||
        (find->Kind == PI5_GRAPH_FIND_PHANDLE && (!find->Phandle || find->Phandle == UINT32_MAX))) return GraphInvalid;
    for (i = find->Start; i < g->Info.Nodes; ++i) {
        const GRAPH_NODE *n = &g->Nodes[i];
        int match = 0;
        if (find->Kind == PI5_GRAPH_FIND_PATH) match = !strcmp(n->Path, find->Text);
        if (find->Kind == PI5_GRAPH_FIND_OWNER) match = !strcmp(n->Owner, find->Text);
        if (find->Kind == PI5_GRAPH_FIND_PHANDLE) match = n->Phandle == find->Phandle;
        if (find->Kind == PI5_GRAPH_FIND_COMPATIBLE) {
            const GRAPH_PROPERTY *p = GraphProperty(g, i, "compatible");
            uint32_t at = 0;
            if (p) while (at < p->Length) {
                const uint8_t *end = memchr(p->Data + at, 0, p->Length - at);
                if (!end) return GraphInvalid;
                if (!strcmp((const char *)p->Data + at, find->Text)) match = 1;
                at = (uint32_t)(end - p->Data) + 1;
            }
        }
        if (match) { *node = i; return GraphOk; }
    }
    return GraphNotFound;
}

GRAPH_RESULT GraphResolve(const GRAPH *g, const PI5_GRAPH_RESOLVE *q, PI5_GRAPH_REFERENCE *reference)
{
    const GRAPH_PROPERTY *p;
    uint32_t at = 0, index = 0, provider, count, phandle, i;
    int found = 0;
    PI5_GRAPH_REFERENCE result = {0};
    if (q->Version != PI5_GRAPH_VERSION || q->Reserved || q->Node >= g->Info.Nodes ||
        !memchr(q->Property, 0, sizeof(q->Property)) || !q->Property[0] ||
        !memchr(q->CellsProperty, 0, sizeof(q->CellsProperty))) return GraphInvalid;
    p = GraphProperty(g, q->Node, q->Property);
    if (!p) return GraphNotFound;
    if (p->Length % 4) return GraphInvalid;
    while (at < p->Length) {
        phandle = Be32(p->Data + at); at += 4;
        if (!phandle) { ++index; continue; }
        for (provider = 0; provider < g->Info.Nodes; ++provider)
            if (g->Nodes[provider].Phandle == phandle) break;
        if (provider == g->Info.Nodes) return GraphInvalid;
        count = 0;
        if (q->CellsProperty[0]) {
            const GRAPH_PROPERTY *cells = GraphProperty(g, provider, q->CellsProperty);
            if (!cells || cells->Length != 4) return GraphInvalid;
            count = Be32(cells->Data);
        }
        if (count > PI5_GRAPH_CELLS || count > (p->Length - at) / 4) return GraphInvalid;
        if (index == q->Index) {
            result.Version = PI5_GRAPH_VERSION;
            result.Provider = provider; result.Phandle = phandle; result.Count = count;
            for (i = 0; i < count; ++i) result.Cells[i] = Be32(p->Data + at + 4 * i);
            found = 1;
        }
        at += 4 * count; ++index;
    }
    if (!found) return GraphNotFound;
    *reference = result;
    return GraphOk;
}
