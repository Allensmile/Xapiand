---
title: Match All Query
---

The simplest query is `_match_all`, which matches all documents, returns all
documents in any given database giving them all a weight of 0.0:

#### Example

{% capture req %}

```json
POST /bank/:search?pretty

{
  "_query": {
    "_match_all": {}
  }
}
```
{% endcapture %}
{% include curl.html req=req %}


# Match None Query

The query `_match_none` is the inverse of the `_match_all` query, and matches
no documents.

#### Example

{% capture req %}

```json
POST /bank/:search?pretty

{
  "_query": {
    "_match_none": {}
  }
}
```
{% endcapture %}
{% include curl.html req=req %}
