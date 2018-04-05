---
title: Backups
---


## Full Dump and Restore

### Dump

A dump of the "twitter" index can be done in three steps:

```sh
# 1. Dump index metadata.
~ $ xapiand --dump-metadata="twitter" --out="twitter.meta"
# 2. Dump index schema.
~ $ xapiand --dump-schema="twitter" --out="twitter.schm"
# 3. Dump index documents.
~ $ xapiand --dump="twitter" --out="twitter.docs"
```

### Restore

The restore for the above dump can also be done in three steps:

```sh
# 1. Restore index metadata.
~ $ xapiand --restore="twitter" --in="twitter.meta"
# 2. Restore index schema.
~ $ xapiand --restore="twitter" --in="twitter.schm"
# 3. Restore index documents.
~ $ xapiand --restore="twitter" --in="twitter.docs"
```


## Restore using different schema

If you need a different or definitive schema for the dumped documents, instead
of restoring the metadata and the schema (steps *1* and *2*, above) you may want
to put a different schema for the index to be restored; and then restore the
documents to that index:

#### Create a new schema ([foreign]({{ '/docs/reference-guide/schema#foreign' | relative_url }}) in this example) for a new index

{% capture json %}

```json
PUT /new_twitter/:schema
{
  "_type": "foreign/object",
  "_endpoint": ".schemas/00000000-0000-1000-8000-010000000000",
  "_id": {
    "_type": "uuid",
  },
  "description": "Twitter Schema",
  "schema": {
    "_type": "object",
    "_id": {
      "_type": "integer",
    },
    "user": {
      "_type": "term"
    },
    "postDate": {
      "_type": "datetime"
    },
    "message": {
      "_type": "text"
    }
  },
}
```
{% endcapture %}
{% include curl.html json=json %}

#### Restore the index documents to the new index

```sh
~ $ xapiand --restore="new_twitter" --in="twitter.docs"
```


## Online Dump and Restore

It's also possible (for rather small databases) to dump and restore all
documents to and from JSON (or MessagePack) over HTTP.

### Dump

{% capture json %}

```json
POST /twitter/:dump?pretty
```
{% endcapture %}
{% include curl.html json=json %}

### Restore

{% capture json %}

```json
POST /twitter/:restore?pretty
[
  {
    "user": "Kronuz",
    "postDate": "2016-11-15T13:12:00",
    "message": "Trying out Xapiand, so far, so good... so what!",
    "_id": 1
  },
  {
    "user": "Kronuz",
    "postDate": "2016-10-15T10:31:18",
    "message": "Another tweet, will it be indexed?",
    "_id": 2
  }
]
```
{% endcapture %}
{% include curl.html json=json %}
