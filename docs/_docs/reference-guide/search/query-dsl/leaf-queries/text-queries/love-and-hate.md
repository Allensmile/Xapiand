---
title: Love and Hate Query
short_title: Love and Hate
---

The `+` and `-` operators, select documents based on the presence or absence of
specified terms.

{: .note .info }
**_Note_**<br>
When using these operators, _stop words_ do not apply.


#### Example

The following matches all documents with the phrase _"adventurous nature"_ but
not _ambitious_; and:

{% capture req %}

```json
SEARCH /bank/

{
  "_query": {
    "personality": "\"adventurous nature\" -ambitious"
  }
}
```
{% endcapture %}
{% include curl.html req=req %}

{% comment %}
---
params: sort=_id
---

```js
pm.test("Response is success", function() {
  pm.response.to.be.success;
});
```

```js
pm.test("Love and Hate count", function() {
  var jsonData = pm.response.json();
  pm.expect(jsonData.count).to.equal(10);
});
```

```js
pm.test("Love and Hate size", function() {
  var jsonData = pm.response.json();
  pm.expect(jsonData.hits.length).to.equal(10);
});
```

```js
pm.test("Love and Hate values are valid", function() {
  var jsonData = pm.response.json();
  var expected = [24, 37, 268, 340, 378, 380, 400, 448, 479, 492];
  for (var i = 0; i < expected.length; ++i) {
    pm.expect(jsonData.hits[i]._id).to.equal(expected[i]);
  }
});
```
{% endcomment %}

{: .note .caution }
**_Caution_**<br>
One thing to note is that the behaviour of the +/- operators vary depending on
the default operator used and the above examples assume that the default (`OR`)
is used.
