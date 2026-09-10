<?php
namespace App\Tests;
use App\Math;
use PHPUnit\Framework\TestCase;
final class MathTest extends TestCase {
    public function testClassifyNegative(): void {
        $this->assertSame('negative', (new Math())->classify(-1));
    }
    public function testClassifyPositive(): void {
        $this->assertSame('positive', (new Math())->classify(5));
    }
    public function testSum(): void {
        $this->assertSame(6, (new Math())->sum([1,2,3]));
    }
}
