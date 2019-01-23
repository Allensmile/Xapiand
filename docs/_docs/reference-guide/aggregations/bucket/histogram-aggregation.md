---
title: Histogram Aggregation
---

A _multi-bucket_ values source based aggregation that can be applied on numeric
values extracted from the documents. It dynamically builds fixed size (a.k.a.
interval) buckets over the values. For example, if the documents have a field
that holds a balance (numeric), we can configure this aggregation to dynamically
build buckets with interval `500` (in case of balance it may represent $500).
When the aggregation executes, the balance field of every document will be
evaluated and will be rounded down to its closest bucket - for example, if the
balance is `3200` and the bucket size is `500` then the rounding will yield
`3000` and thus the document will "fall" into the bucket that is associated with
the key `3000`. To make this more formal, here is the rounding function that is
used:

```cpp
bucket_key = floor((value - _shift) / _interval) * _interval + _shift;
```


## Structuring

The following snippet captures the structure of histogram aggregations:

```json
"<aggregation_name>": {
  "_histogram": {
      "_field": "<field_name>",
      "_interval": "<interval>",
      ( "_shift": <shift> )?
  },
  ...
}
```

### Field

The `<field_name>` in the `_field` parameter defines the field on which the
aggregation will act upon.

### Interval

The `_interval` must be a positive decimal, while the `_shift` must be a decimal
in `[0, _interval)` (a decimal greater than or equal to `0` and less than
`_interval`)

Assuming the data consists of documents representing bank accounts, as shown in
the sample dataset of [Exploring Your Data]({{ '/docs/exploring/' | relative_url }}#sample-dataset)
section, the following snippet "buckets" the bank accounts based on their
`balance` by interval of `500`:

{% capture req %}

```json
POST /bank/:search?pretty

{
  "_query": "*",
  "_limit": 0,
  "_check_at_least": 1000,
  "_aggs": {
    "balances": {
      "_histogram": {
        "_field": "balance",
        "_interval": 500
      }
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

And the following may be the response:

```json
  "#aggregations": {
    "_doc_count": 1000,
    "balances": [
      {
        "_doc_count": 179,
        "_key": "3000.0"
      },
      {
        "_doc_count": 174,
        "_key": "2500.0"
      },
      {
        "_doc_count": 167,
        "_key": "2000.0"
      },
      {
        "_doc_count": 165,
        "_key": "1500.0"
      },
      {
        "_doc_count": 162,
        "_key": "3500.0"
      },
      {
        "_doc_count": 153,
        "_key": "1000.0"
      }
    ]
  },
  ...
```

### Shift

By default the bucket keys start with 0 and then continue in even spaced steps
of interval, e.g. if the interval is 10 the first buckets (assuming there is
data inside them) will be [0, 10), [10, 20), [20, 30). The bucket boundaries
can be shifted by using the `_shift` option.

This can be best illustrated with an example. If there are 10 documents with
values ranging from 5 to 14, using interval 10 will result in two buckets with
5 documents each. If an additional shift of 5 is used, there will be only one
single bucket [5, 15) containing all the 10 documents.


### Ordering

By default, the returned buckets are sorted by their `_key` ascending, though
the order behaviour can be controlled using the `_sort` setting. Supports the
same order functionality as explained in [Bucket Ordering](..#ordering).
