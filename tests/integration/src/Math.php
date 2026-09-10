<?php
namespace App;
final class Math {
    public function classify(int $n): string {
        if ($n < 0) {
            return 'negative';
        } elseif ($n === 0) {
            return 'zero';
        }
        return 'positive';
    }
    public function sum(array $xs): int {
        $t = 0;
        foreach ($xs as $x) {
            $t += $x;
        }
        return $t;
    }
}
