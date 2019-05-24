---
title: Sum Aggregation
short_title: Sum
---

A _single-value_ metrics aggregation that sums up numeric values that are
extracted from the aggregated documents.

## Structuring

The following snippet captures the structure of sum aggregations:

```json
"<aggregation_name>": {
  "_sum": {
    "_field": "<field_name>"
  },
  ...
}
```

### Field

The `<field_name>` in the `_field` parameter defines the specific field from
which the numeric values in the documents are extracted.

Assuming the data consists of documents representing bank accounts, as shown in
the sample dataset of [Data Exploration]({{ '/docs/exploration' | relative_url }}#sample-dataset)
section, we can sum the balances of all accounts in the state of Indiana with:

{% capture req %}

```json
SEARCH /bank/

{
  "_query": {
    "contact.state": "Indiana"
  },
  "_limit": 0,
  "_aggs": {
    "indiana_total_balance": {
      "_sum": {
        "_field": "balance"
      }
    }
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

{: .test }

```js
pm.test("Response is success", function() {
  pm.response.to.be.success;
});
```

{: .test }

```js
pm.test("Response is aggregation", function() {
  var jsonData = pm.response.json();
  function expectEqualNumbers(a, b) {
    pm.expect(Math.round(parseFloat(a) * 1000) / 1000).to.equal(Math.round(parseFloat(b) * 1000) / 1000);
  }
  expectEqualNumbers(jsonData.aggregations.indiana_total_balance._sum, 42152.87);
});
```

Resulting in:

```json
{
  "aggregations": {
    "_doc_count": 17,
    "indiana_total_balance": {
      "_sum": 42152.87
    }
  }, ...
}
```
