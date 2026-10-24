//! Just enough XML for the DSN Now files: elements with attributes, in order. Comments, the declaration and CDATA are skipped; text
//! is the raw text after a start tag up to the next `<` (only `<timestamp>` uses it). No validation: a malformed tag ends the scan.

/// One tag. `attrs` is the raw attribute text of a start tag; `text` the raw text that follows the tag.
pub struct Tag<'a> {
    pub name: &'a str,
    pub attrs: &'a str,
    pub text: &'a str,
    pub closing: bool,
    pub self_closing: bool,
}

pub struct Tags<'a> {
    s: &'a str,
    pos: usize,
}

impl<'a> Tags<'a> {
    pub fn new(s: &'a str) -> Self {
        Tags { s, pos: 0 }
    }
}

impl<'a> Iterator for Tags<'a> {
    type Item = Tag<'a>;

    fn next(&mut self) -> Option<Tag<'a>> {
        loop {
            let rest = &self.s[self.pos..];
            let lt = rest.find('<')?;
            let start = self.pos + lt;
            let after = &self.s[start..];
            // skip <!-- -->, <![CDATA[ ]]>, <!DOCTYPE>, <? ?>
            let skip_to = if after.starts_with("<!--") {
                after.find("-->").map(|i| i + 3)
            } else if after.starts_with("<![CDATA[") {
                after.find("]]>").map(|i| i + 3)
            } else if after.starts_with("<!") || after.starts_with("<?") {
                after.find('>').map(|i| i + 1)
            } else {
                None
            };
            if after.starts_with("<!") || after.starts_with("<?") {
                self.pos = start + skip_to?;
                continue;
            }
            let gt = tag_end(after)?;
            let inner = &after[1..gt];
            self.pos = start + gt + 1;
            let closing = inner.starts_with('/');
            let body = if closing { &inner[1..] } else { inner };
            let self_closing = body.ends_with('/');
            let body = body.strip_suffix('/').unwrap_or(body);
            let name_end = body.find(|c: char| c.is_ascii_whitespace()).unwrap_or(body.len());
            let name = &body[..name_end];
            let attrs = &body[name_end..];
            let tail = &self.s[self.pos..];
            let text = &tail[..tail.find('<').unwrap_or(tail.len())];
            if name.is_empty() {
                return None;
            }
            return Some(Tag { name, attrs, text, closing, self_closing });
        }
    }
}

/// The index of the `>` that ends a tag, ignoring any inside quoted attribute values.
fn tag_end(s: &str) -> Option<usize> {
    let mut quote: Option<u8> = None;
    for (i, &b) in s.as_bytes().iter().enumerate().skip(1) {
        match quote {
            Some(q) if b == q => quote = None,
            Some(_) => {}
            None if b == b'"' || b == b'\'' => quote = Some(b),
            None if b == b'>' => return Some(i),
            None if b == b'<' => return None,
            None => {}
        }
    }
    None
}

/// The raw value of an attribute (entities not decoded), None when absent.
pub fn attr<'a>(attrs: &'a str, key: &str) -> Option<&'a str> {
    let b = attrs.as_bytes();
    let mut i = 0;
    while i < b.len() {
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        let ks = i;
        while i < b.len() && b[i] != b'=' && !b[i].is_ascii_whitespace() {
            i += 1;
        }
        let k = &attrs[ks..i];
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= b.len() || b[i] != b'=' {
            if k.is_empty() {
                return None;
            }
            continue; // an attribute without a value
        }
        i += 1;
        while i < b.len() && b[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= b.len() {
            return None;
        }
        let (vs, ve) = if b[i] == b'"' || b[i] == b'\'' {
            let q = b[i];
            let vs = i + 1;
            let ve = vs + b[vs..].iter().position(|&c| c == q)?;
            i = ve + 1;
            (vs, ve)
        } else {
            let vs = i;
            while i < b.len() && !b[i].is_ascii_whitespace() {
                i += 1;
            }
            (vs, i)
        };
        if k == key {
            return Some(&attrs[vs..ve]);
        }
    }
    None
}
