:- module(test_subprop,
	  [ test/1,			% +Times
	    replay/0,
	    replay/1			% +File
	  ]).
:- use_module(library(semweb/rdf_db)).
:- use_module(library(record)).
:- use_module(library(debug)).
:- use_module(library(broadcast)).
:- use_module(random_graph).
:- use_module(search).

/** <module> Test RDF subproperty handling

This design assumes N properties with a   randomly changing set of edges
between them. Edges are  created  at  a   generation  and  killed  at  a
generation.  We maintain a shadow Prolog DB to verify the correctness of
the inferences made by the RDF DB.

The idea is to have a set   of properties and pseudo-randomly create and
remove subPropertyOf relations. For each  property,   we  have  a single
triple that has the same URI as the property: rdf(P,P,P). The properties
are named p1, p2, ... pN.
*/

:- debug(subprop).

:- dynamic
	predicate/3,			% Predicate, Gen, Graph
	sub_of/4,			% P1, P2, Born, Graph
	snap/4,				% SnapID, Gen, Snap, Graph
	died/3.				% Born, Died, Graph

cleanup(G) :-
	retractall(predicate(_,_,G)),
	retractall(sub_of(_,_,_,G)),
	retractall(died(_,_,G)),
	retractall(snap(_,_,_,G)),
%	rdf_gc,
	rdf_reset_db.

test(N) :-
	test(g1, N).

test(G, N) :-
	atom_concat(G, '.rec', LogFile),
	setup_call_cleanup(
	    record_in(LogFile, LogStream),
	    run_test(G, N),
	    close_recording(LogStream)).

run_test(G, N) :-
	show_graph(G),
	graph_settings(G,
		       [ verify(0.01),
			 create_snap(0.1),
			 verify_snap(0.01),
			 delete_snap(0.05)
		       ]),
	reset_graph(G),
	setup_call_cleanup(
	    listen(G, graph(G,Action), update_graph_true(G, Action)),
	    loop(1, N, G),
	    unlisten(G)).

loop(I, I, _) :- !.
loop(I, N, G) :-
	graph_steps(G,1),
	succ(I, I2),
	format(user_error, '\r~t~D~6|', [I]),
	loop(I2, N, G).


update_graph_true(G, Action) :-
	debug(subprop, '~q: ~p', [G, Action]),
	update_graph(G, Action), !.
update_graph_true(_, _Action) :-
	assertion(false).

update_graph(G, reset) :-
	cleanup(G).
update_graph(G, verify) :-
	check_all(G).
update_graph(G, create_snap(SnapId)) :-
	rdf_snapshot(Snap),
	rdf_generation(Gen),
	assertz(snap(SnapId, Gen, Snap, G)).
update_graph(G, verify_snap(SnapId)) :-
	snap(SnapId, Gen, Snap, G),
	rdf_transaction(check_all(Gen), _Id, [snapshot(Snap)]).
update_graph(G, delete_snap(SnapId)) :-
	retract(snap(SnapId, _Gen, Snap, G)),
	rdf_delete_snapshot(Snap).
update_graph(G, add_node(I)) :-
	atom_concat(p, I, P),
	rdf_statistics(triples(T0)),
	rdf_assert(P,P,P),
	rdf_statistics(triples(T1)),
	assertion(T0+1 =:= T1),
	rdf_generation(Gen),
	assertz(predicate(P, Gen, G)).
update_graph(G, add_edge(SubI,SuperI)) :-
	atom_concat(p, SubI, Sub),
	atom_concat(p, SuperI, Super),
	rdf_assert(Sub, rdfs:subPropertyOf, Super),
	rdf_generation(Gen),
	assertz(sub_of(Sub,Super,Gen,G)).
update_graph(G, del_edge(SubI,SuperI)) :-
	atom_concat(p, SubI, Sub),
	atom_concat(p, SuperI, Super),
	rdf_retractall(Sub, rdfs:subPropertyOf, Super),
	rdf_generation(Gen),
	forall((sub_of(Sub,Super,Born,G),
		\+ died(Born, _, G)),
	       assertz(died(Born, Gen,G))).

check_all(Graph) :-
	rdf_generation(Gen),
	check_all(Graph, Gen).

check_all(Graph, Gen) :-
	forall(visible_predicate(Graph, Gen, P1),
	       forall(visible_predicate(Graph, Gen, P2),
		      check_all(Graph, Gen, P1, P2))).

check_all(Graph, Gen, Sub, Super) :-
	(   subPropertyOf(Graph, Gen, Sub, Super)
	->  assertion(rdf_has(Sub, Super, Sub))
	;   assertion(\+ rdf_has(Sub, Super, Sub))
	).



visible_predicate(Graph, Gen, P) :-
	predicate(P, Born, Graph),
	Gen >= Born,
	\+ (  died(Born, Died, Graph),
	      Gen >= Died
	   ).

subPropertyOf_1(Graph, Gen, Sub, Super) :-
	sub_of(Sub, Super, Born, Graph),
	Gen >= Born,
	\+ (  died(Born, Died, Graph),
	      Gen >= Died
	   ).

superPropertyOf_1(Graph, Gen, Super, Sub) :-
	subPropertyOf_1(Graph, Gen, Sub, Super).


%%	subPropertyOf(+Gen, ?Sub, ?Super) is nondet.
%
%	True when Node1 and Node2 are   connected, considering the graph
%	as an undirected graph.

subPropertyOf(Graph, Gen, Sub, Super) :-
	nonvar(Sub), nonvar(Super), !,
	bf_expand(subPropertyOf_1(Graph, Gen), Sub, [Super|_]), !.
subPropertyOf(Graph, Sub, Super, Gen) :-
	nonvar(Sub), !,
	bf_expand(subPropertyOf_1(Graph, Gen), Sub, [Super|_]).
subPropertyOf(Graph, Sub, Super, Gen) :-
	nonvar(Super), !,
	bf_expand(superPropertyOf_1(Graph, Gen), Super, [Sub|_]).


		 /*******************************
		 *	       RECORD		*
		 *******************************/

:- dynamic
	record_stream/1.

record_in(File, Out) :-
	open(File, write, Out),
	asserta(record_stream(Out)),
	listen(record, graph(G, Action), save(G, Action)).

:- at_halt(close_recording(_)).

close_recording(Out) :-
	forall(retract(record_stream(Out)),
	       close(Out)).

save(Graph, Action) :-
	record_stream(Out),
	format(Out, 'action(~q, ~q).~n', [Graph, Action]),
	flush_output(Out).

%%	replay
%
%	Replay the last randomly generated suite

replay :-
	replay(g1).

replay(Graph) :-
	file_name_extension(Graph, rec, File),
	open(File, read, In),
	repeat,
	    read(In, Term),
	    (	Term == end_of_file
	    ->	!, close(In)
	    ;	Term = action(Graph,Action),
		broadcast(graph(Graph, Action)),
		check_all(Graph),
		fail
	    ).
