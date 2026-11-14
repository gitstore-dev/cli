package main

import (
	"io"
	"net/http"
	"strings"
	"testing"
)

type routeFixture func(*http.Request) (*http.Response, error)

func (f routeFixture) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestRoutingEvidenceRecordsOnlyHttpsOrigins(t *testing.T) {
	transport := &routingTransport{origins: map[string]bool{}, redirects: map[string]bool{}, base: routeFixture(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 307, Header: http.Header{"Location": []string{"https://blob.example/private/path?token=synthetic-secret"}}, Body: io.NopCloser(strings.NewReader("")), Request: r}, nil
	})}
	request, _ := http.NewRequest("GET", "https://registry.example/v2/private?token=synthetic-secret", nil)
	response, err := transport.RoundTrip(request)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if !transport.origins["https://registry.example"] || !transport.redirects["https://blob.example"] || len(transport.origins) != 1 || len(transport.redirects) != 1 {
		t.Fatal("unexpected origin projection")
	}
	for _, bad := range []string{"http://registry.example/", "https://user:synthetic-secret@registry.example/"} {
		request, _ = http.NewRequest("GET", bad, nil)
		if _, err := transport.RoundTrip(request); err == nil {
			t.Fatal("unsafe source allowed")
		}
	}
}
