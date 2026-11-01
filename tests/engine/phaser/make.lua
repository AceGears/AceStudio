local d = ace.new_document(16, 16, 6)
for f = 0, 5 do d:set_cel(0, f, ace.image(16, 16, f < 4 and '#ff8800' or '#2288ff')) end
d:set_duration(0, 120)
d:add_tag('walk', 0, 3)
d:add_tag('hit', 4, 5)
d:save('hero.acestudio')
