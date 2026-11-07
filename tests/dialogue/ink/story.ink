// A small story in Ink, the subset tools/ink_to_yarn.py converts.
VAR gold = 3
VAR met = false

Mika: Welcome to the market. # happy
-> shop

=== shop ===
Mika: What will it be? You have {gold} gold.
* [Ask about the pies] Mika: Fresh this morning.
    ~ met = true
    -> shop
+ {gold >= 2} [Buy a pie]
    ~ gold = gold - 2
    Mika: Enjoy it!
    -> shop
+ [Leave] -> goodbye
- Mika: Hm?

=== goodbye ===
{met: Mika: Come back for more stories.|Mika: Bye, stranger.}
-> END
