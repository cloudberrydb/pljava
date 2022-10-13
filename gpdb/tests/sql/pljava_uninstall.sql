-- start_ignore
-- FIXME: if manually build pljava, can we find it in gppkg?
\! gppkg -q --all | sed -n '/^pljava-/s|.*|pljava|p'
\! gppkg -r pljava
-- end_ignore
\! gppkg -q --all | sed -n '/^pljava-/s|.*|pljava|p'
