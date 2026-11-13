# Security Policy

## Reporting a vulnerability

Please do not publish exploitable details in a public issue. Contact the repository owner privately through GitHub.

## Current protections

- Only HTTP and HTTPS input URLs are accepted.
- User-info credentials in URLs are rejected.
- Hostnames are resolved before extraction; loopback, private, link-local, multicast and reserved IP ranges are rejected.
- Request bodies are limited to 16 KiB.
- Per-IP parsing is rate-limited and global extraction concurrency is capped.
- The extractor process has a hard execution timeout and an output-size limit.
- The browser UI does not inject title, description or uploader strings as trusted HTML.

## Known limitations

DNS answers can change after validation, and extractors may follow redirects to other hosts. The current implementation is therefore best-effort SSRF protection, not a network sandbox. For a public production deployment, add outbound network policy that blocks internal/private destinations, an upstream WAF or authentication, monitoring, and stricter per-user quotas.

Do not store personal cookies or session tokens in this public service. This project does not implement cookie import or authentication bypass.
