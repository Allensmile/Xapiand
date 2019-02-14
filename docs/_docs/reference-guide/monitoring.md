---
title: Monitoring Xapiand
short_title: Monitoring
---

You can retrieve information about the server usage by using the `:metrics`
endpoint.

This returns a _Prometheus_ compatible response with a bunch of metrics.

{% capture req %}

```json
GET /:metrics
```
{% endcapture %}
{% include curl.html req=req %}


<div style="min-height: 800px"></div>
