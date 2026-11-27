package homeassistant

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

// An encrypted ESPHome frame carries at most 65,515 bytes, and Home Assistant does not check: a
// larger action response goes out with its length wrapped, the device cannot decrypt the frame and
// the connection drops. Anything that can grow past this is fetched in pages.
const pageBudget = 40000

// PageBudget is the most bytes of items one page carries, for templates outside this package.
const PageBudget = pageBudget

const pageTemplate = `{%- set ns = namespace(seen=0, parts=[], size=2, next=none) -%}
{%- for s in SOURCE if WHERE -%}
{%- if ns.seen >= START -%}
PREPARE
{%- set j = ITEM | to_json(ensure_ascii=True) -%}
{%- if j | length > BUDGET -%}
{%- set j = SMALL | to_json(ensure_ascii=True) -%}
{%- endif -%}
{%- if ns.parts and ns.size + (j | length) + 1 > BUDGET -%}
{%- set ns.next = ns.seen -%}
{%- break -%}
{%- endif -%}
{%- set ns.parts = ns.parts + [j] -%}
{%- set ns.size = ns.size + (j | length) + 1 -%}
{%- endif -%}
{%- set ns.seen = ns.seen + 1 -%}
{%- endfor -%}
{"next": {{ ns.next | to_json }}, "items": [{{ ns.parts | join(',') }}]}`

type listing struct {
	source  string
	where   string
	prepare string
	item    string
	small   string
}

func (l listing) page(start int) string {
	where := l.where
	if where == "" {
		where = "true"
	}
	return strings.NewReplacer(
		"SOURCE", l.source,
		"WHERE", where,
		"START", strconv.Itoa(start),
		"PREPARE", l.prepare,
		"ITEM", l.item,
		"SMALL", l.small,
		"BUDGET", strconv.Itoa(pageBudget),
	).Replace(pageTemplate)
}

type render func(ctx context.Context, tmpl string) (json.RawMessage, error)

func listAll[T any](ctx context.Context, ask render, l listing) ([]T, error) {
	return Paged[T](ctx, func(ctx context.Context, start int) (json.RawMessage, error) {
		return ask(ctx, l.page(start))
	})
}

// Paged asks for one page after another, each answered as {"next": <start or null>, "items": [...]},
// until a page has no next.
func Paged[T any](
	ctx context.Context,
	ask func(ctx context.Context, start int) (json.RawMessage, error),
) ([]T, error) {
	out := []T{}
	for start := 0; ; {
		raw, err := ask(ctx, start)
		if err != nil {
			return nil, err
		}
		var page struct {
			Next  *int `json:"next"`
			Items []T  `json:"items"`
		}
		if err := json.Unmarshal(unwrap(raw), &page); err != nil {
			return nil, fmt.Errorf("homeassistant: page at %d: %w", start, err)
		}
		out = append(out, page.Items...)
		if page.Next == nil {
			return out, nil
		}
		if *page.Next <= start {
			return nil, fmt.Errorf("homeassistant: page at %d continues at %d", start, *page.Next)
		}
		start = *page.Next
	}
}
