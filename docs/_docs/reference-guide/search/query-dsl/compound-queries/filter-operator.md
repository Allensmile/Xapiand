---
title: Filter Operator
---

A query can be filtered by another query. There are two ways to apply a filter
to a query depending whether you want to include or exclude documents:

* `_filter`       - Matches documents which match both subqueries, but the
                    weight is only taken from the left subquery (in other
                    respects it acts like `_and`.
* `_and_not`      - Matches documents which match the left subquery but don’t
                    match the right hand one (with weights coming from the left
                    subquery)

#### Example

For example, the following matches all who like _bananas_ filtering the results
to those who also are _brown-eyed females_, but this filter doesn't affect
weights:

{% capture req %}

```json
SEARCH /bank/

{
  "_query": {
    "_filter": [
      {
        "favoriteFruit": "banana"
      },
      {
        "_and": [
            { "gender": "female" },
            { "eyeColor": "brown" }
        ]
      }
    ]
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

{: .test }

```js
pm.test("response is success", function() {
  pm.response.to.be.success;
});
```

{: .test }

```js
pm.test("Filter Operator count", function() {
  var jsonData = pm.response.json();
  pm.expect(jsonData.count).to.equal(10);
});
```

{: .test }

```js
pm.test("Filter Operator size", function() {
  var jsonData = pm.response.json();
  pm.expect(jsonData.hits.length).to.equal(10);
});
```

{: .test }

```js
pm.test("Filter Operator values are valid", function() {
  var jsonData = pm.response.json();
  var expected = [665, 319, 735, 475, 934, 969, 284, 576, 999, 417];
  for (var i = 0; i < 10; ++i) {
    pm.expect(jsonData.hits[i]._id).to.equal(expected[i]);
  }
});
```
