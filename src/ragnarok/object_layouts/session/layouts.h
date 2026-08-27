#pragma once

// La disposition mémoire de CSession, par version de client.
//
// 🔴 IL N'EN RESTE QUE DEUX, et ce n'est pas un oubli. Trois autres ont vécu ici
// (2015-11-02, 2017-06-13, 2019-01-16), avec de vrais offsets relevés — mais
// elles ne couvraient que la SESSION. Le reste du projet appelle 407 adresses
// en dur, toutes propres au 20250716 : un client plus ancien franchissait la
// porte, puis s'écroulait au premier hook. Elles promettaient un support qui
// n'existait pas. Retirées le 2026-08-26 ; git les garde si le besoin renaît,
// mais ce sont alors les 400 autres qu'il faudra relever, pas ce fichier-ci.
#include "ragnarok/object_layouts/session/20250716.h"
#include "ragnarok/object_layouts/session/20260707.h"
