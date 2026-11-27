package calendar

import (
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ygelfand/libcountertop/pkg/homeassistant"
)

func TestEventsPageAsksFromItsStartWithinTheBudget(t *testing.T) {
	page := eventsPage(140)
	if strings.Contains(page, "START") || strings.Contains(page, "BUDGET") {
		t.Fatalf("placeholders left in the template:\n%s", page)
	}
	if !strings.Contains(page, "ns.seen >= 140") {
		t.Error("the page does not start at 140")
	}
	if !strings.Contains(page, "> "+strconv.Itoa(homeassistant.PageBudget)) {
		t.Error("the page is not bounded by homeassistant.PageBudget")
	}
}

func TestParseKeepsEachEventsCalendarAndOrder(t *testing.T) {
	loc := time.UTC
	got := parse([]wire{
		{
			Calendar: "calendar.work",
			Start:    "2026-11-27T10:00:00-07:00",
			End:      "2026-11-27T11:00:00-07:00",
			Summary:  "standup",
		},
		{Calendar: "calendar.home", Start: "2026-11-27", End: "2026-11-28", Summary: "holiday"},
		{Calendar: "calendar.home", Start: "not a time", Summary: "dropped"},
		{
			Calendar:    "calendar.home",
			Start:       "2026-11-26T09:00:00-07:00",
			End:         "",
			Summary:     "no end",
			Description: "notes",
		},
	}, loc)
	if len(got) != 3 {
		t.Fatalf("parsed %d events, want 3: %+v", len(got), got)
	}
	want := []struct {
		calendar, summary string
		allDay            bool
	}{
		{"calendar.home", "no end", false},
		{"calendar.home", "holiday", true},
		{"calendar.work", "standup", false},
	}
	for i, w := range want {
		e := got[i]
		if e.Calendar != w.calendar || e.Summary != w.summary || e.AllDay != w.allDay {
			t.Errorf("event %d = %s %q all-day=%v, want %s %q all-day=%v",
				i, e.Calendar, e.Summary, e.AllDay, w.calendar, w.summary, w.allDay)
		}
	}
	if !got[0].End.Equal(got[0].Start) || got[0].Description != "notes" {
		t.Errorf("an event without an end should end at its start and keep its notes: %+v", got[0])
	}
}
