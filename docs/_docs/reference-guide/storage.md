---
title: Storage API
---

The storage is designed to put files in volumes much in the way Facebook's
Haystack <sup>[1](#footnote-1)</sup> works; once there a file enters the
storage it can't really be deleted/modified from the volume, but instead, if
a change is needed, a new file blob will be written to the volume. Storage is
envisioned to be used when there are files you need to store which you know
won't be changing often or at all.

Assuming there is a [PNG]({{ '/assets/Lenna.png' | absolute_url }}){:target="_blank"} file called `Lenna.png`
in the working directory, lets add those to the storage using `STORE`:

{% capture req %}

```json
STORE /assets/Lenna
Content-Type: image/png

@Lenna.png
```
{% endcapture %}
{% include curl.html req=req %}

And getting it is just a matter of retreiving it using the `GET` HTTP method:

{% capture req %}

```json
GET /assets/Lenna
Accept: image/png
```
{% endcapture %}
{% include curl.html req=req %}

Or by visiting the link to it with your web browser:
[http://localhost:8880/assets/Lenna](http://localhost:8880/assets/Lenna){:target="_blank"}

{: .note .tip }
**_Toggle Console Previews_**<br>
You can enable previews for images in the terminal using the "_very-very-very-very_"
verbose command line option (`-vvvvv`). Note you a compatible terminal for this
feature to work ([iTerm2](https://www.iterm2.com){:target="_blank"}, for example).

## Multi-Content Documents

Use `STORE` with a different `Content-Type` to add new content to the same
document. For example, the following adds a [PDF]({{ '/assets/Lenna.pdf' | absolute_url }}){:target="_blank"}
and a [JPEG]({{ '/assets/Lenna.jpg' | absolute_url }}){:target="_blank"} from
files called `Lenna.pdf` and `Lenna.jpg`, respectively:

{% capture req %}

```json
STORE /assets/Lenna
Content-Type: application/pdf

@Lenna.pdf
```
{% endcapture %}
{% include curl.html req=req %}

{% capture req %}

```json
STORE /assets/Lenna
Content-Type: image/jpeg

@Lenna.jpg
```
{% endcapture %}
{% include curl.html req=req %}

Then you can get either of them requesting the appropriate content type in
the `Accept` header:

{% capture req %}

```json
GET /assets/Lenna
Accept: application/pdf
```
{% endcapture %}
{% include curl.html req=req %}


{: .note .info }
**_Default Content Type_**<br>
In Multi-Content Documents, the last content that was stored is the
_default content type_, if none is specified using the `Accept` header.


## Retrieving Information

You can get the information about the document as usual:

{% capture req %}

```json
GET /assets/:info/Lenna?pretty
```
{% endcapture %}
{% include curl.html req=req %}

The result (partially shown) has the available content types listed inside
 `document_info ➛ data`

```json
{
  "document_info": {
    "docid": 1,
    "data": [
      {
        "content_type": "application/msgpack",
        "type": "inplace"
      },
      {
        "content_type": "image/png",
        "type": "stored",
        "volume": 0,
        "offset": 512,
        "size": "462.7KiB"
      },
      {
        "content_type": "application/pdf",
        "type": "stored",
        "volume": 0,
        "offset": 60063,
        "size": "288.2KiB"
      },
      {
        "content_type": "image/jpeg",
        "type": "stored",
        "volume": 0,
        "offset": 94433,
        "size": "557.6KiB"
      }
    ], ...
  }
}
```

## Removing Content

To remove stored content by storing an empty object:

{% capture req %}

```json
STORE /assets/Lenna
Content-Type: image/jpeg
Content-Length: 0
```
{% endcapture %}
{% include curl.html req=req %}

Note removing content doesn't actually remove the blob from the volume, it
just removes the "link" to it from the document.

---

<sup><a id="footnote-1">1</a></sup> [Finding a needle in Haystack: Facebook's photo storage.](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Beaver.pdf){:target="_blank"}
