--TEST--
Check if post hook can returned modified clone
----DESCRIPTION--
A different object might be returned than the one provided to post hook. For example, PSR-7 messages are immutable and modifying
one creates a new instance.
--EXTENSIONS--
opentelemetry
--FILE--
<?php
class Foo
{
    public ?string $a = null;
    public function __construct(string $a = null)
    {
        $this->a = $a;
    }
    public function modify(string $value): Foo
    {
        $new = clone($this);
        $new->a = $value;

        return $new;
    }
}

\OpenTelemetry\Instrumentation\hook(null, 'getFoo', null, function(null $obj, array $params, Foo $foo): Foo {
    return $foo->modify('b');
});

function getFoo(): Foo {
    return new Foo('a'); //providing a constructor arg or not sometimes changes error to a reported memory leak
}

var_dump(getFoo());
?>
--EXPECT--
object(Foo)#2 (1) {
  ["a"]=>
  string(1) "b"
}
